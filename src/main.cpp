// In-circuit capacitor checker, 2-WIRE variant: ESP32-C3 + 0.42" OLED + Wi-Fi web interface. See ../README.md.
//  BOOT button (screens in screens.h): TAP = test / next / cancel, HOLD (0.7 s) = menu / accept / back, HOLD 10 s = screen fit.
//  A test starts when the probes are sensed on the capacitor (or after a countdown / immediately: menu > GO).
//  Web interface (join Wi-Fi "CapChecker", open http://192.168.4.1): tests with error bars, the full calibration wizard
//  (open, short, known resistor, known capacitor) and learned good parts.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_continuous.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "capdsp.h"
#include "capjson.h"
#include "oled.h"
#include "screens.h"
#include "web_ui.h"

static const char* TAG = "cap";

// ---- pin map and options (change here or with -D flags in platformio.ini) ----
#define PIN_DRIVE 3   // LEDC square wave -> 330R -> clips
#define PIN_BTN 9     // on-board BOOT button (active low)
#ifndef WIFI_ENABLED
#define WIFI_ENABLED 1
#endif
#ifndef AP_SSID
#define AP_SSID "CapChecker"
#endif
#ifndef AP_PASS
#define AP_PASS "capcheck1"   // WPA2 needs >= 8 characters
#endif
// ADC1 channel == GPIO number on the C3:  GPIO0 = node A (clips, AC coupled) , GPIO4 = drive reference tap
static const adc_channel_t CH_PAD = ADC_CHANNEL_0, CH_REF = ADC_CHANNEL_4;

static const double NOMINALS[] = {0, 0.47, 1, 2.2, 3.3, 4.7, 10, 22, 33, 47, 100, 220, 330, 470, 1000, 2200, 3300, 4700, 10000};
static const int NNOM = sizeof(NOMINALS) / sizeof(NOMINALS[0]);

static Oled oled;
static volatile bool g_cancel = false;        // set by a button tap or the web page: abort the running job
static uint16_t g_buf[2 * cap::MAX_FRAMES];  // interleaved frames [node A, ref]
static double g_w[cap::MAX_FRAMES];           // window scratch
static cap::Cal g_cal;                        // calibration (protected by g_mu)
static cap::RefTable g_refs;                  // learned good parts (protected by g_mu)
static SemaphoreHandle_t g_mu;

// ---------------------------------------------------------------- acquisition: LEDC drive + ADC continuous
static volatile bool g_adc_ovf = false;
static bool IRAM_ATTR on_pool_ovf(adc_continuous_handle_t, const adc_continuous_evt_data_t*, void*) {
  g_adc_ovf = true;
  return false;
}

struct EspAcq {
  adc_continuous_handle_t adc = nullptr;
  bool primed = false;
  double f_last = 0;

  void init() {
    adc_continuous_handle_cfg_t hc = {};
    hc.max_store_buf_size = 32768;  // holds a whole 4096-frame capture, so Wi-Fi activity cannot make the ADC drop samples
    hc.conv_frame_size = 1200;
    ESP_ERROR_CHECK(adc_continuous_new_handle(&hc, &adc));
    adc_continuous_evt_cbs_t cbs = {};
    cbs.on_pool_ovf = on_pool_ovf;
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(adc, &cbs, nullptr));

    ledc_channel_config_t ch = {};
    ch.gpio_num = PIN_DRIVE;
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.channel = LEDC_CHANNEL_0;
    ch.timer_sel = LEDC_TIMER_0;
    ch.duty = 0;
    ch.hpoint = 0;
    ch.intr_type = LEDC_INTR_DISABLE;
    set_freq(1000);  // timer must exist before the channel is configured
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    drive_off();
  }

  void set_freq(double f) {
    uint32_t res = ledc_find_suitable_duty_resolution(80000000, (uint32_t)f);
    if (res > 14) res = 14;
    ledc_timer_config_t t = {};
    t.speed_mode = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = (ledc_timer_bit_t)res;
    t.timer_num = LEDC_TIMER_0;
    t.freq_hz = (uint32_t)f;
    t.clk_cfg = LEDC_USE_APB_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    f_last = ledc_get_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0);
    duty_ = (1u << res) / 2;
    if (primed) {
      ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_);
      ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
  }
  void drive_off() {
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);  // pin parked LOW: node A sits at 0 V, discharges the DUT
    primed = false;
  }
  double f_actual(double) { return f_last; }

  bool get(const cap::Plan& p, uint16_t* buf) {
    if (g_cancel) return false;
    set_freq(p.f);
    if (!primed) {  // first start: wait for the coupling caps (tau ~0.2 s) to settle on the DC step
      ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_);
      ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
      primed = true;
      vTaskDelay(pdMS_TO_TICKS(700));
    } else {
      vTaskDelay(pdMS_TO_TICKS(120));
    }
    for (int attempt = 0; attempt < 3 && !g_cancel; attempt++) {  // a capture with a dropped sample is useless: retry
      g_adc_ovf = false;
      if (capture(p, buf) && !g_adc_ovf) return true;
    }
    return false;
  }

 private:
  bool capture(const cap::Plan& p, uint16_t* buf) {
    adc_digi_pattern_config_t pat[2] = {};
    const adc_channel_t chs[2] = {CH_PAD, CH_REF};
    for (int i = 0; i < 2; i++) {
      pat[i].atten = ADC_ATTEN_DB_2_5;
      pat[i].channel = chs[i];
      pat[i].unit = ADC_UNIT_1;
      pat[i].bit_width = ADC_BITWIDTH_12;
    }
    adc_continuous_config_t cfg = {};
    cfg.pattern_num = 2;
    cfg.adc_pattern = pat;
    cfg.sample_freq_hz = (uint32_t)(2 * p.frame_rate + 0.5);
    cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    cfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
    if (adc_continuous_config(adc, &cfg) != ESP_OK) return false;
    if (adc_continuous_start(adc) != ESP_OK) return false;

    static uint8_t rb[1200];
    int frames = 0, pos = 0, skip = 4;
    uint16_t cur[2] = {0, 0};
    const int64_t t_end = esp_timer_get_time() + 4000000LL + (int64_t)(p.n / p.frame_rate * 2e6);
    while (frames < p.n && esp_timer_get_time() < t_end) {
      uint32_t len = 0;
      esp_err_t r = adc_continuous_read(adc, rb, sizeof rb, &len, 50);
      if (r != ESP_OK && r != ESP_ERR_TIMEOUT) break;
      for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= len && frames < p.n; i += SOC_ADC_DIGI_RESULT_BYTES) {
        const adc_digi_output_data_t* d = (const adc_digi_output_data_t*)&rb[i];
        const int c = d->type2.channel;
        const int idx = (c == CH_PAD) ? 0 : (c == CH_REF) ? 1 : -1;
        if (idx < 0) continue;
        if (idx != pos) {  // lost sync: restart the frame on node A
          pos = 0;
          if (idx != 0) continue;
        }
        cur[pos++] = d->type2.data;
        if (pos == 2) {
          pos = 0;
          if (skip > 0) { skip--; continue; }
          buf[2 * frames] = cur[0];
          buf[2 * frames + 1] = cur[1];
          frames++;
        }
      }
    }
    adc_continuous_stop(adc);
    adc_continuous_flush_pool(adc);
    return frames == p.n;
  }
  uint32_t duty_ = 0;
};

static EspAcq acq;

// ---------------------------------------------------------------- preferences (survive power cycles)
struct Prefs {
  uint32_t magic;
  int32_t ni, lvl, smode, delay_s, xoff, yoff, flip;
};
static const uint32_t PREFS_MAGIC = 0x50524633;
static Prefs g_pf = {PREFS_MAGIC, 0, cap::LV_NORMAL, 0, 5, OLED_XOFF, OLED_YOFF, 0};  // smode: 0 = when probes connect, 1 = after delay, 2 = now

// ---------------------------------------------------------------- persistence
static void load_all() {
  nvs_handle_t h;
  if (nvs_open("cap", NVS_READONLY, &h) != ESP_OK) return;
  size_t sz = sizeof g_cal;
  cap::Cal c;
  if (nvs_get_blob(h, "cal3", &c, &sz) == ESP_OK && sz == sizeof c && c.magic == cap::CAL_MAGIC) g_cal = c;
  size_t sz2 = sizeof g_refs;
  cap::RefTable t;
  if (nvs_get_blob(h, "refs3", &t, &sz2) == ESP_OK && sz2 == sizeof t && t.n >= 0 && t.n <= cap::MAX_REFS) g_refs = t;
  size_t sz3 = sizeof g_pf;
  Prefs p;
  if (nvs_get_blob(h, "prefs3", &p, &sz3) == ESP_OK && sz3 == sizeof p && p.magic == PREFS_MAGIC && p.ni >= 0 && p.ni < NNOM && p.lvl >= 0 && p.lvl <= 2 &&
      p.smode >= 0 && p.smode <= 2 && p.delay_s >= 1 && p.delay_s <= 30 && p.xoff >= 0 && p.xoff <= 56 && p.yoff >= 0 && p.yoff <= 24)
    g_pf = p;
  nvs_close(h);
}
static void save_all() {  // caller holds g_mu
  nvs_handle_t h;
  if (nvs_open("cap", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, "cal3", &g_cal, sizeof g_cal);
  nvs_set_blob(h, "refs3", &g_refs, sizeof g_refs);
  nvs_commit(h);
  nvs_close(h);
}
static void save_prefs() {
  nvs_handle_t h;
  if (nvs_open("cap", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, "prefs3", &g_pf, sizeof g_pf);
  nvs_commit(h);
  nvs_close(h);
}

// ---------------------------------------------------------------- job worker (button and web both queue jobs here)
enum Kind { K_TEST, K_OPEN, K_SHORT, K_RES, K_CAP, K_LEARN, K_QUICKCAL };
struct JobReq { Kind kind; double val; double nom; int lvl; int smode; int delay_s; };
enum Phase { PH_NONE, PH_WAIT, PH_COUNT, PH_MEASURE };
static QueueHandle_t g_q;
static volatile bool g_running = false;
static volatile int g_seq = 0;          // incremented for every submitted job (lets the UI notice jobs that finish within one tick)
static volatile int g_phase = PH_NONE, g_cnt = 0, g_good = 0;
static const char* g_kind = "";
static volatile int g_pi = 0, g_pn = 1;
static bool g_ok = false, g_has_result = false;
static char g_msg[200] = "";
static char g_json[2800] = "";
static cap::Report g_rep;               // last test report (shown on the OLED result screen)
static double g_rep_nom = 0;
static volatile bool g_new_result = false;
static const char* g_title = "MEASURING";  // OLED title during measurement
static char g_l1[16] = "KEEP PROBES", g_l2[16] = "ON THE CAP";

static double now_s() { return esp_timer_get_time() * 1e-6; }

static void nominal_str(char* o, size_t n, int ni) { ui::fmt_nominal(o, n, NOMINALS[ni]); }

static void progress(int i, int n) {
  static int64_t last = 0;
  g_pi = i;
  g_pn = n > 0 ? n : 1;
  const int64_t now = esp_timer_get_time();
  if (now - last < 300000 && i < n) return;  // do not let OLED writes slow the measurement
  last = now;
  ui::measuring(oled, g_title, g_l1, g_l2, i, n > 0 ? n : 1);
  oled.flush();
}

static void finish(bool ok, const char* msg) {
  xSemaphoreTake(g_mu, portMAX_DELAY);
  g_ok = ok;
  snprintf(g_msg, sizeof g_msg, "%s", msg);
  xSemaphoreGive(g_mu);
}
static void snapshot(cap::Cal& c, cap::RefTable& t) {
  xSemaphoreTake(g_mu, portMAX_DELAY);
  c = g_cal;
  t = g_refs;
  xSemaphoreGive(g_mu);
}
static void msg_screen(const char* title, const char* a, const char* b = "", const char* c = "", const char* d = "") {
  ui::message(oled, title, a, b, c, d);
  oled.flush();
}
static void set_text(const char* title, const char* l1, const char* l2) {
  g_title = title;
  snprintf(g_l1, sizeof g_l1, "%.12s", l1);
  snprintf(g_l2, sizeof g_l2, "%.12s", l2);
}

// countdown with cancel. returns false if cancelled
static bool count_down(const char* title, const char* line, int secs) {
  g_phase = PH_COUNT;
  for (int s = secs; s >= 1; s--) {
    g_cnt = s;
    ui::countdown(oled, title, line, s);
    oled.flush();
    for (int k = 0; k < 10; k++) {
      if (g_cancel) return false;
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
  return !g_cancel;
}

// Wait for the operator according to the start mode:  0 = until the probes are on a part and still, 1 = fixed delay, 2 = go now.
// Returns true when the measurement may start; otherwise the job's result message has been set and the OLED shows why.
static bool arm(int smode, int delay_s, const cap::Cal& cal, const char* title, const char* nomtxt, const char* what) {
  g_phase = PH_NONE;
  g_good = 0;
  if (smode == 2) return true;
  if (smode == 1) {
    if (!count_down(title, "GET READY", delay_s)) { msg_screen("CANCELLED", "STOPPED", "", "", "TAP: HOME"); finish(false, "Cancelled."); return false; }
    return true;
  }
  g_phase = PH_WAIT;
  char tt[40];
  snprintf(tt, sizeof tt, "%.5s %.7s", what, nomtxt);
  static int tick = 0;
  const cap::WaitResult w = cap::wait_connected(
      acq, cal, g_buf, g_w, 60.0, now_s, [] { return (bool)g_cancel; },
      [&](int good, int need, double) {
        g_good = good;
        ui::wait_probes(oled, tt, good, need, tick++);
        oled.flush();
      });
  acq.drive_off();
  g_phase = PH_NONE;
  switch (w) {
    case cap::WAIT_OK: return true;
    case cap::WAIT_CANCEL: msg_screen("CANCELLED", "STOPPED", "", "", "TAP: HOME"); finish(false, "Cancelled."); break;
    case cap::WAIT_TIMEOUT:
      msg_screen("NO CAP SEEN", "60 S PASSED", "TINY CAP? USE", "MENU > GO NOW", "TAP: HOME");
      finish(false, "No capacitor detected within 60 s. A part below about 0.16 uF is not detected: use the 'immediately' or 'after 5 s' start mode.");
      break;
    default: msg_screen("NO SIGNAL", "CHECK DRIVE", "AND ADC WIRES", "", "TAP: HOME"); finish(false, "No signal: check the drive and ADC wiring."); break;
  }
  return false;
}

static void cancelled() {
  acq.drive_off();
  msg_screen("CANCELLED", "STOPPED", "", "", "TAP: HOME");
  finish(false, "Cancelled.");
}

static void run_job(const JobReq& q) {
  static cap::Cal cal;       // static: keeps the worker stack small
  static cap::RefTable refs;
  char msg[200] = "";
  char nt[24];
  ui::fmt_nominal(nt, sizeof nt, q.nom);
  snapshot(cal, refs);
  g_has_result = false;
  g_phase = PH_NONE;
  set_text("MEASURING", "KEEP PROBES", "ON THE CAP");
  switch (q.kind) {
    case K_TEST:
    case K_LEARN: {
      g_kind = q.kind == K_TEST ? "test" : "learn";
      if (!cal.ready()) {
        msg_screen("NEED CAL!", "CLIPS APART,", "TAP HOME,", "THEN TAP", "");
        finish(false, "Calibrate first: steps 1 (open) and 2 (shorted) on the Calibrate tab.");
        break;
      }
      const bool learn = q.kind == K_LEARN;
      if (!arm(q.smode, q.delay_s, cal, learn ? "LEARN" : "TEST", nt, learn ? "LEARN" : "TEST")) break;
      g_phase = PH_MEASURE;
      cap::Report r = cap::run_test(acq, cal, learn ? nullptr : &refs, q.nom, learn ? cap::LV_PRECISE : q.lvl, g_buf, g_w, progress);
      acq.drive_off();
      g_phase = PH_NONE;
      if (g_cancel) { cancelled(); break; }
      ESP_LOGI(TAG, "%s", r.why);
      if (learn) {
        const bool near = std::isfinite(r.c_uF) && r.c_uF > 0 && fabs(log(r.c_uF / q.nom)) < log(1.5);
        if (!cap::ref_acceptable(r)) {
          msg_screen("NOT LEARNED", r.l1, r.l2, "CHECK PART", "TAP: HOME");
          snprintf(msg, sizeof msg, "Not learned: reading is %s. Check the part and the clips.", cap::CODE_NAME[r.code]);
          finish(false, msg);
        } else if (!near) {
          msg_screen("NOT LEARNED", "FAR FROM", "NOMINAL", "", "TAP: HOME");
          snprintf(msg, sizeof msg, "Not learned: measured %.3g uF is far from the %.3g uF you entered (wrong value, or a failed part).", r.c_uF, q.nom);
          finish(false, msg);
        } else {
          xSemaphoreTake(g_mu, portMAX_DELAY);
          const bool ok = cap::ref_learn(g_refs, q.nom, r.c_uF, r.esr);
          if (ok) save_all();
          const int i = cap::ref_index(g_refs, q.nom);
          const int n = i >= 0 ? g_refs.r[i].n : 0;
          xSemaphoreGive(g_mu);
          msg_screen(ok ? "LEARNED" : "LIST FULL", nt, r.l3, "", "TAP: HOME");
          snprintf(msg, sizeof msg, ok ? "Learned %.3g uF: C = %.4g uF, ESR = %.3f ohm (%d part%s averaged)." : "Reference list is full (16): delete one first.",
                   q.nom, r.c_uF, r.esr, n, n == 1 ? "" : "s");
          finish(ok, msg);
        }
      } else {
        cap::JB j(g_json, sizeof g_json);
        cap::json_report(j, r, q.nom, q.lvl);
        g_has_result = true;
        g_rep = r;
        g_rep_nom = q.nom;
        g_new_result = true;
        finish(r.code != cap::V_ERROR, r.code == cap::V_ERROR ? r.why : "");
      }
      break;
    }
    case K_OPEN: {
      g_kind = "open";
      set_text("CAL 1/2", "CLIPS APART", "DONT TOUCH");
      if (q.smode != 2 && !count_down("CAL 1/2", "CLIPS APART", q.smode == 1 ? q.delay_s : 4)) { cancelled(); break; }
      g_phase = PH_MEASURE;
      const bool ok = cap::calibrate_open(acq, cal, g_buf, g_w, progress);
      acq.drive_off();
      if (g_cancel) { cancelled(); break; }
      if (!ok) { msg_screen("CAL FAILED", "STEP 1", "CHECK WIRING", "", "TAP: HOME"); finish(false, "No signal: check the drive and ADC wiring, and that the clips are apart."); break; }
      xSemaphoreTake(g_mu, portMAX_DELAY);
      g_cal = cal;
      save_all();
      xSemaphoreGive(g_mu);
      msg_screen("CAL 1/2 OK", "NOW: CLIP", "THE CLIPS", "TOGETHER", "TAP: HOME");
      finish(true, "Open reference stored. Steps 2-4 were cleared: continue with step 2.");
      break;
    }
    case K_SHORT: {
      g_kind = "short";
      set_text("CAL 2/2", "CLIPS TOGETHER", "FIRM CONTACT");
      if (!(cal.flags & cap::F_OPEN)) { msg_screen("CAL 2/2", "DO STEP 1", "FIRST", "", "TAP: HOME"); finish(false, "Do step 1 (clips open) first."); break; }
      if (!arm(q.smode, q.delay_s, cal, "CAL 2/2", "", "SHORT")) break;
      g_phase = PH_MEASURE;
      const bool ok = cap::calibrate_short(acq, cal, g_buf, g_w, progress);
      acq.drive_off();
      if (g_cancel) { cancelled(); break; }
      if (!ok) { msg_screen("CAL FAILED", "STEP 2", "CLIPS NOT", "SHORTED?", "TAP: HOME"); finish(false, "Clips not shorted (or no signal). Clip them firmly together and retry."); break; }
      xSemaphoreTake(g_mu, portMAX_DELAY);
      g_cal = cal;
      save_all();
      xSemaphoreGive(g_mu);
      snprintf(msg, sizeof msg, "Leads: %.3f ohm at 1 kHz. Calibrated: the checker is ready to test.", cal.z0[cap::PROBE_IDX].real());
      msg_screen("CAL OK", "SAVED", "READY TO TEST", "", "TAP: HOME");
      finish(true, msg);
      break;
    }
    case K_RES: {
      g_kind = "res";
      char rt[24];
      snprintf(rt, sizeof rt, "%gR", q.val);
      set_text("CAL RES", rt, "ON THE CLIPS");
      if (!cal.ready()) { finish(false, "Do steps 1 and 2 first."); break; }
      if (!arm(q.smode, q.delay_s, cal, "CAL RES", rt, "RES")) break;
      g_phase = PH_MEASURE;
      double rm = 0;
      const int rc = cap::calibrate_res(acq, cal, q.val, g_buf, g_w, progress, &rm);
      acq.drive_off();
      if (g_cancel) { cancelled(); break; }
      if (rc == 0) {
        xSemaphoreTake(g_mu, portMAX_DELAY);
        g_cal = cal;
        save_all();
        xSemaphoreGive(g_mu);
        snprintf(msg, sizeof msg, "Read %.3f ohm for a %.3g ohm resistor: gain trim set to %.4f.", rm, q.val, cal.rtrim);
        msg_screen("CAL RES OK", rt, "TRIM SAVED", "", "TAP: HOME");
        finish(true, msg);
      } else if (rc == 2) {
        snprintf(msg, sizeof msg, "Read %.3f ohm but you entered %.3g ohm (more than 25%% off, or not resistive). Check the value and the clip contact.", rm, q.val);
        msg_screen("CAL REJECT", "VALUE OFF", "BY >25%", "", "TAP: HOME");
        finish(false, msg);
      } else { msg_screen("CAL FAILED", "NO SIGNAL", "", "", "TAP: HOME"); finish(false, "No signal."); }
      break;
    }
    case K_CAP: {
      g_kind = "cap";
      char ct[24];
      ui::fmt_nominal(ct, sizeof ct, q.val);
      set_text("CAL CAP", ct, "ON THE CLIPS");
      if (!cal.ready()) { finish(false, "Do steps 1 and 2 first."); break; }
      if (!arm(q.smode, q.delay_s, cal, "CAL CAP", ct, "CAP")) break;
      g_phase = PH_MEASURE;
      double cm = 0;
      const int rc = cap::calibrate_cap(acq, cal, q.val, g_buf, g_w, progress, &cm);
      acq.drive_off();
      if (g_cancel) { cancelled(); break; }
      if (rc == 0) {
        xSemaphoreTake(g_mu, portMAX_DELAY);
        g_cal = cal;
        save_all();
        xSemaphoreGive(g_mu);
        const int s = q.val <= 3.0 ? 0 : 1;
        snprintf(msg, sizeof msg, "Read %.4g uF for a %.4g uF capacitor: C scale set to %.4f (%s range).", cm, q.val, cal.ctrim[s], s ? "large" : "small");
        msg_screen("CAL CAP OK", ct, "SCALE SAVED", "", "TAP: HOME");
        finish(true, msg);
      } else if (rc == 2) {
        snprintf(msg, sizeof msg, "Read %.4g uF but you entered %.4g uF (more than 30%% off): check the value, the part and the clips.", cm, q.val);
        msg_screen("CAL REJECT", "VALUE OFF", "BY >30%", "", "TAP: HOME");
        finish(false, msg);
      } else { msg_screen("CAL FAILED", "NOT A CAP OR", "NO SIGNAL", "", "TAP: HOME"); finish(false, "Not a capacitor, or no signal."); }
      break;
    }
    case K_QUICKCAL: {  // OLED guided calibration: clips apart (countdown), then clips together (waits for contact)
      g_kind = "quickcal";
      set_text("CAL 1/2", "CLIPS APART", "DONT TOUCH");
      if (!count_down("CAL 1/2", "CLIPS APART", 5)) { cancelled(); break; }
      g_phase = PH_MEASURE;
      bool ok = cap::calibrate_open(acq, cal, g_buf, g_w, progress);
      if (g_cancel) { cancelled(); break; }
      if (!ok) { acq.drive_off(); msg_screen("CAL FAILED", "STEP 1", "CHECK WIRING", "ADC/DRIVE", "TAP: HOME"); finish(false, "Open calibration failed."); break; }
      set_text("CAL 2/2", "CLIPS TOGETHER", "FIRM CONTACT");
      if (!arm(0, 0, cal, "CAL 2/2", "", "SHORT")) break;
      g_phase = PH_MEASURE;
      ok = cap::calibrate_short(acq, cal, g_buf, g_w, progress);
      acq.drive_off();
      if (g_cancel) { cancelled(); break; }
      if (!ok) { msg_screen("CAL FAILED", "STEP 2", "CLIPS NOT", "SHORTED?", "TAP: HOME"); finish(false, "Short calibration failed."); break; }
      xSemaphoreTake(g_mu, portMAX_DELAY);
      g_cal = cal;
      save_all();
      xSemaphoreGive(g_mu);
      msg_screen("CAL OK", "SAVED", "READY TO TEST", "", "TAP: HOME");
      finish(true, "Calibrated.");
      break;
    }
  }
}

static void job_task(void*) {
  JobReq q;
  for (;;) {
    if (xQueueReceive(g_q, &q, portMAX_DELAY) != pdTRUE) continue;
    run_job(q);
    g_pi = g_pn;
    g_phase = PH_NONE;
    g_running = false;
  }
}

// returns false if a job is already running
static bool submit(Kind k, double val, double nom, int lvl, int smode, int delay_s) {
  if (g_running) return false;
  g_running = true;
  g_seq = g_seq + 1;
  g_cancel = false;
  g_phase = PH_NONE;
  g_pi = 0;
  g_pn = 1;
  g_good = 0;
  g_has_result = false;
  g_ok = false;
  g_msg[0] = 0;
  const JobReq q = {k, val, nom, lvl, smode, delay_s};
  if (xQueueSend(g_q, &q, 0) != pdTRUE) { g_running = false; return false; }
  return true;
}

// ---------------------------------------------------------------- pending display adjustments requested by the web page
static volatile bool g_disp_req = false;
static volatile int g_disp_dx = 0, g_disp_dy = 0, g_disp_flip = 0, g_disp_reset = 0;

// ---------------------------------------------------------------- web interface
#if WIFI_ENABLED
static volatile bool g_wifi_on = false;
static esp_err_t send_json(httpd_req_t* r, const char* s) {
  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  return httpd_resp_send(r, s, HTTPD_RESP_USE_STRLEN);
}
static bool qparam(httpd_req_t* r, const char* key, char* out, size_t n) {
  char q[128];
  if (httpd_req_get_url_query_str(r, q, sizeof q) != ESP_OK) return false;
  return httpd_query_key_value(q, key, out, n) == ESP_OK;
}
static double qnum(httpd_req_t* r, const char* key, double dflt) {
  char b[24];
  return qparam(r, key, b, sizeof b) ? atof(b) : dflt;
}

static esp_err_t h_index(httpd_req_t* r) {
  httpd_resp_set_type(r, "text/html");
  return httpd_resp_send(r, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t h_status(httpd_req_t* r) {
  static char buf[3200];
  static cap::Cal c;
  static cap::RefTable t;
  snapshot(c, t);
  cap::JB j(buf, sizeof buf);
  j.raw("{\"cal\":"); cap::json_cal(j, c);
  j.raw(",\"refs\":"); cap::json_refs(j, t);
  j.f(",\"busy\":%s,\"wifi\":true,\"disp\":{\"x\":%d,\"y\":%d,\"flip\":%d}}", g_running ? "true" : "false", (int)g_pf.xoff, (int)g_pf.yoff, (int)g_pf.flip);
  return send_json(r, buf);
}
static esp_err_t h_job(httpd_req_t* r) {
  static char buf[3300];
  cap::JB j(buf, sizeof buf);
  xSemaphoreTake(g_mu, portMAX_DELAY);
  const bool running = g_running;
  j.f("{\"state\":\"%s\",\"kind\":\"%s\",\"phase\":%d,\"cnt\":%d,\"good\":%d,\"need\":%d,\"i\":%d,\"n\":%d,\"ok\":%s,\"msg\":", running ? "running" : "done", g_kind,
      (int)g_phase, (int)g_cnt, (int)g_good, cap::CONNECT_STABLE, (int)g_pi, (int)g_pn, g_ok ? "true" : "false");
  j.str(g_msg);
  j.raw(",\"result\":");
  if (!running && g_has_result) j.raw(g_json); else j.raw("null");
  j.raw("}");
  xSemaphoreGive(g_mu);
  return send_json(r, buf);
}
static esp_err_t h_start(httpd_req_t* r) {
  char kind[12] = "";
  if (!qparam(r, "kind", kind, sizeof kind)) return send_json(r, "{\"ok\":false,\"msg\":\"missing kind\"}");
  const double val = qnum(r, "val", 0), nom = qnum(r, "nom", 0);
  const int lvl = (int)qnum(r, "lvl", 1), sm = (int)qnum(r, "sm", 0), dl = (int)qnum(r, "dl", 5);
  Kind k;
  if (!strcmp(kind, "test")) k = K_TEST;
  else if (!strcmp(kind, "open")) k = K_OPEN;
  else if (!strcmp(kind, "short")) k = K_SHORT;
  else if (!strcmp(kind, "res")) { k = K_RES; if (!(val >= 0.5 && val <= 50)) return send_json(r, "{\"ok\":false,\"msg\":\"Resistor must be 0.5 to 50 ohm\"}"); }
  else if (!strcmp(kind, "cap")) { k = K_CAP; if (!(val >= 0.1 && val <= 100000)) return send_json(r, "{\"ok\":false,\"msg\":\"Capacitor must be 0.1 to 100000 uF\"}"); }
  else if (!strcmp(kind, "learn")) { k = K_LEARN; if (!(nom >= 0.1 && nom <= 100000)) return send_json(r, "{\"ok\":false,\"msg\":\"Enter the nominal value (0.1 to 100000 uF)\"}"); }
  else return send_json(r, "{\"ok\":false,\"msg\":\"unknown kind\"}");
  if (lvl < 0 || lvl > 2 || sm < 0 || sm > 2 || dl < 1 || dl > 30) return send_json(r, "{\"ok\":false,\"msg\":\"bad option\"}");
  if (!submit(k, val, nom, lvl, sm, dl)) return send_json(r, "{\"ok\":false,\"msg\":\"The checker is busy: wait for the running measurement.\"}");
  return send_json(r, "{\"ok\":true}");
}
static esp_err_t h_cancel(httpd_req_t* r) {
  g_cancel = true;
  return send_json(r, "{\"ok\":true}");
}
static esp_err_t h_disp(httpd_req_t* r) {
  if (g_running) return send_json(r, "{\"ok\":false}");
  g_disp_dx = (int)qnum(r, "dx", 0);
  g_disp_dy = (int)qnum(r, "dy", 0);
  g_disp_flip = (int)qnum(r, "flip", 0);
  g_disp_reset = (int)qnum(r, "reset", 0);
  g_disp_req = true;
  return send_json(r, "{\"ok\":true}");
}
static esp_err_t h_ref_del(httpd_req_t* r) {
  xSemaphoreTake(g_mu, portMAX_DELAY);
  if (cap::ref_delete(g_refs, (int)qnum(r, "i", -1))) save_all();
  xSemaphoreGive(g_mu);
  return send_json(r, "{\"ok\":true}");
}
static esp_err_t h_reset(httpd_req_t* r) {
  if (g_running) return send_json(r, "{\"ok\":false}");
  xSemaphoreTake(g_mu, portMAX_DELAY);
  g_cal = cap::Cal();
  if (qnum(r, "all", 0) > 0) g_refs = cap::RefTable();
  save_all();
  xSemaphoreGive(g_mu);
  return send_json(r, "{\"ok\":true}");
}
static esp_err_t h_wifi_off(httpd_req_t* r) {
  send_json(r, "{\"ok\":true}");
  vTaskDelay(pdMS_TO_TICKS(300));
  g_wifi_on = false;
  esp_wifi_stop();
  return ESP_OK;
}

static void wifi_web_start() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_ap();  // 192.168.4.1
  wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wc));
  wifi_config_t ap = {};
  strncpy((char*)ap.ap.ssid, AP_SSID, sizeof ap.ap.ssid);
  strncpy((char*)ap.ap.password, AP_PASS, sizeof ap.ap.password);
  ap.ap.ssid_len = strlen(AP_SSID);
  ap.ap.channel = 1;
  ap.ap.max_connection = 2;
  ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap.ap.beacon_interval = 1000;  // fewer radio bursts while measuring
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
  ESP_ERROR_CHECK(esp_wifi_start());
  g_wifi_on = true;

  httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
  hc.stack_size = 8192;
  hc.max_uri_handlers = 12;
  hc.lru_purge_enable = true;
  httpd_handle_t srv = nullptr;
  ESP_ERROR_CHECK(httpd_start(&srv, &hc));
  const httpd_uri_t uris[] = {
      {"/", HTTP_GET, h_index, nullptr},           {"/api/status", HTTP_GET, h_status, nullptr},   {"/api/job", HTTP_GET, h_job, nullptr},
      {"/api/start", HTTP_POST, h_start, nullptr}, {"/api/cancel", HTTP_POST, h_cancel, nullptr}, {"/api/disp", HTTP_POST, h_disp, nullptr},
      {"/api/ref/del", HTTP_POST, h_ref_del, nullptr}, {"/api/reset", HTTP_POST, h_reset, nullptr}, {"/api/wifi/off", HTTP_POST, h_wifi_off, nullptr}};
  for (const auto& u : uris) ESP_ERROR_CHECK(httpd_register_uri_handler(srv, &u));
  ESP_LOGI(TAG, "web interface: join Wi-Fi '%s' (password '%s'), open http://192.168.4.1", AP_SSID, AP_PASS);
}
#else
static const bool g_wifi_on = false;
#endif

// ---------------------------------------------------------------- button: TAP (short press), HOLD (0.7 s, fires while still pressed), FAR (10 s)
enum Ev { EV_NONE, EV_TAP, EV_HOLD, EV_FAR };
static Ev poll_button() {
  static bool down = false, fired_hold = false, fired_far = false;
  static int64_t t0 = 0;
  const bool pressed = !gpio_get_level((gpio_num_t)PIN_BTN);
  const int64_t now = esp_timer_get_time();
  if (pressed && !down) { down = true; fired_hold = fired_far = false; t0 = now; return EV_NONE; }
  if (pressed && down) {
    if (!fired_hold && now - t0 > 700000) { fired_hold = true; return EV_HOLD; }
    if (!fired_far && now - t0 > 10000000) { fired_far = true; return EV_FAR; }
    return EV_NONE;
  }
  if (!pressed && down) {
    down = false;
    if (!fired_hold && now - t0 > 30000) return EV_TAP;
  }
  return EV_NONE;
}

// ---------------------------------------------------------------- OLED user interface
enum Scr { S_HOME, S_MENU, S_NOM, S_WIFI, S_RESULT, S_MSG, S_SETUP };
static Scr scr = S_HOME;
static int menu_sel = 0, wifi_page = 0, setup_stage = 0;
static int64_t t_scr = 0;
static const char* const SMODE_NAME[] = {"GO ON CAP", "GO IN 5S", "GO NOW"};
static const char* const LVL_NAME[] = {"ACC QUICK", "ACC NORMAL", "ACC PRECISE"};
static const int NMENU = 7;
static const char* const IP_STR = "192.168.4.1";

static void menu_labels(char items[][16]) {
  char n[24];
  nominal_str(n, sizeof n, g_pf.ni);
  snprintf(items[0], 16, "NOM %.8s", n);
  snprintf(items[1], 16, "%s", LVL_NAME[g_pf.lvl]);
  if (g_pf.smode == 1) snprintf(items[2], 16, "GO IN %dS", (int)g_pf.delay_s);
  else snprintf(items[2], 16, "%s", SMODE_NAME[g_pf.smode]);
  snprintf(items[3], 16, "WIFI INFO");
  snprintf(items[4], 16, "CALIBRATE");
  snprintf(items[5], 16, "SCREEN FIT");
  snprintf(items[6], 16, "BACK");
}

static void go(Scr s) {
  scr = s;
  t_scr = esp_timer_get_time();
}

static void apply_screen() {
  oled.xoff = g_pf.xoff;
  oled.yoff = g_pf.yoff;
  oled.set_flip(g_pf.flip != 0);
}

static void draw() {
  const int64_t now = esp_timer_get_time();
  const int sec = (int)((now - t_scr) / 1000000);
  switch (scr) {
    case S_HOME: {
      char n[24];
      nominal_str(n, sizeof n, g_pf.ni);
      xSemaphoreTake(g_mu, portMAX_DELAY);
      const bool ok = g_cal.ready();
      xSemaphoreGive(g_mu);
      ui::home(oled, n, ok, g_wifi_on && ((now / 2500000) & 1), IP_STR);
      break;
    }
    case S_MENU: {
      char items[NMENU][16];
      menu_labels(items);
      const char* p[NMENU];
      for (int i = 0; i < NMENU; i++) p[i] = items[i];
      ui::menu(oled, p, NMENU, menu_sel);
      break;
    }
    case S_NOM: {
      char n[24];
      nominal_str(n, sizeof n, g_pf.ni);
      ui::pick_nominal(oled, n);
      break;
    }
    case S_WIFI: ui::wifi(oled, wifi_page, g_wifi_on, AP_SSID, AP_PASS, IP_STR); break;
    case S_RESULT: ui::result(oled, g_rep, g_rep_nom, ui::result_has_details(g_rep) ? (sec / 3) & 1 : 0); break;
    case S_MSG: return;  // the worker drew it
    case S_SETUP: ui::screen_setup(oled, setup_stage, g_pf.xoff, g_pf.yoff, g_pf.flip != 0); break;
  }
  oled.flush();
}

static void start_test() { submit(K_TEST, 0, NOMINALS[g_pf.ni], g_pf.lvl, g_pf.smode, g_pf.delay_s); }

static void on_event(Ev ev) {
  switch (scr) {
    case S_HOME:
      if (ev == EV_TAP) {
        xSemaphoreTake(g_mu, portMAX_DELAY);
        const bool ok = g_cal.ready();
        xSemaphoreGive(g_mu);
        if (ok) start_test(); else submit(K_QUICKCAL, 0, 0, 0, 0, 0);
      } else if (ev == EV_HOLD) { menu_sel = 0; go(S_MENU); }
      break;
    case S_RESULT:
      if (ev == EV_TAP) start_test();
      else if (ev == EV_HOLD) { menu_sel = 0; go(S_MENU); }
      break;
    case S_MSG: go(S_HOME); break;
    case S_MENU:
      if (ev == EV_TAP) menu_sel = (menu_sel + 1) % NMENU;
      else if (ev == EV_HOLD) {
        switch (menu_sel) {
          case 0: go(S_NOM); break;
          case 1: g_pf.lvl = (g_pf.lvl + 1) % 3; save_prefs(); break;
          case 2: g_pf.smode = (g_pf.smode + 1) % 3; save_prefs(); break;
          case 3: wifi_page = 0; go(S_WIFI); break;
          case 4: submit(K_QUICKCAL, 0, 0, 0, 0, 0); break;
          case 5: setup_stage = 0; go(S_SETUP); break;
          default: go(S_HOME); break;
        }
      }
      break;
    case S_NOM:
      if (ev == EV_TAP) g_pf.ni = (g_pf.ni + 1) % NNOM;
      else if (ev == EV_HOLD) { save_prefs(); go(S_HOME); }
      break;
    case S_WIFI:
      if (ev == EV_TAP) wifi_page = (wifi_page + 1) % 3;
      else if (ev == EV_HOLD) go(S_HOME);
      break;
    case S_SETUP:
      if (ev == EV_TAP) {
        if (setup_stage == 0) g_pf.yoff = (g_pf.yoff + 1) % 25;
        else if (setup_stage == 1) g_pf.xoff = (g_pf.xoff + 1) % 57;
        else g_pf.flip = !g_pf.flip;
        apply_screen();
      } else if (ev == EV_HOLD) {
        if (++setup_stage > 2) { save_prefs(); go(S_HOME); }
      }
      break;
  }
}

extern "C" void app_main(void) {
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << PIN_BTN;
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&io);

  g_mu = xSemaphoreCreateMutex();
  load_all();
  if (!oled.begin(OLED_SDA, OLED_SCL)) ESP_LOGE(TAG, "OLED init failed");
  apply_screen();
  ui::splash(oled);
  oled.flush();
  acq.init();
  g_q = xQueueCreate(1, sizeof(JobReq));
  xTaskCreate(job_task, "job", 12288, nullptr, 5, nullptr);
#if WIFI_ENABLED
  wifi_web_start();
#endif
  vTaskDelay(pdMS_TO_TICKS(1200));

  // start-up: show how to reach the web page (tap to skip)
  if (g_wifi_on) {
    for (int page = 0; page < 2; page++) {
      ui::wifi(oled, page, true, AP_SSID, AP_PASS, IP_STR);
      oled.flush();
      for (int k = 0; k < 200; k++) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (poll_button() != EV_NONE) break;
      }
    }
  }
  go(S_HOME);

  int seen_seq = 0;
  int64_t last_draw = 0;
  for (;;) {
    const Ev ev = poll_button();
    if (g_disp_req && !g_running) {  // display adjustment asked from the web page
      g_disp_req = false;
      if (g_disp_reset) { g_pf.xoff = OLED_XOFF; g_pf.yoff = OLED_YOFF; g_pf.flip = 0; }
      g_pf.xoff = (g_pf.xoff + g_disp_dx + 57) % 57;
      g_pf.yoff = (g_pf.yoff + g_disp_dy + 25) % 25;
      if (g_disp_flip) g_pf.flip = !g_pf.flip;
      apply_screen();
      save_prefs();
      setup_stage = 0;
      go(S_SETUP);
    }
    if (g_running) {
      if (ev == EV_TAP || ev == EV_HOLD) g_cancel = true;  // any press stops the running job
    } else {
      if (seen_seq != g_seq) {  // a job has finished since the last look
        seen_seq = g_seq;
        if (g_new_result) { g_new_result = false; go(S_RESULT); }
        else go(S_MSG);
        last_draw = 0;
      } else if (ev == EV_FAR) { setup_stage = 0; go(S_SETUP); }
      else if (ev != EV_NONE) { on_event(ev); last_draw = 0; }
      const int64_t now = esp_timer_get_time();
      if (!g_running && now - last_draw > 200000) { draw(); last_draw = now; }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
