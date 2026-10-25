/*
 * ============================================================================
 *  Non-blocking WiFi Reconnection State Machine
 * ============================================================================
 *  States: DISCONNECTED -> CONNECTING -> CONNECTED
 *                 ^                          |
 *                 |                          v (link drops)
 *              WAIT_RETRY  <---------- (connect attempt times out)
 *
 *  Every state transition is driven by millis()-comparisons only -- no
 *  blocking delays, no blocking WiFi.waitForConnectResult(). Failed connection
 *  attempts back off exponentially (capped) so a persistent outage does
 *  not hammer the AP/radio with reconnect attempts every loop tick.
 *
 *  Hardware access (WiFi.begin(), WiFi.status(), WiFi.RSSI()) is injected
 *  as plain function pointers rather than called directly, so this class
 *  has zero dependency on <WiFi.h> and can be exercised with mocks on a
 *  host machine. The demo .ino wires real ESP32 WiFi calls in three
 *  one-line wrapper functions.
 * ============================================================================
 */

#ifndef WIFI_RECONNECT_SM_H
#define WIFI_RECONNECT_SM_H

#include <stdint.h>

enum class WiFiConnState : uint8_t {
  DISCONNECTED = 0,
  CONNECTING,
  CONNECTED,
  WAIT_RETRY
};

// RSSI quality bands. Typical, widely-used rules of thumb for 2.4 GHz
// WiFi: better than -60 dBm is a strong/reliable link, -60 to -75 dBm is
// usable but marginal, worse than -75 dBm is poor and prone to dropped
// packets/retries. These are conservative, commonly cited defaults
// (see report for the full justification) and are exposed as constants
// so they can be tuned per deployment/antenna setup.
enum class RssiQuality : uint8_t { GOOD, FAIR, POOR };

inline RssiQuality classifyRssi(int rssiDbm) {
  if (rssiDbm >= -60) return RssiQuality::GOOD;
  if (rssiDbm >= -75) return RssiQuality::FAIR;
  return RssiQuality::POOR;
}

inline const char *rssiQualityName(RssiQuality q) {
  switch (q) {
    case RssiQuality::GOOD: return "GOOD";
    case RssiQuality::FAIR: return "FAIR";
    default: return "POOR";
  }
}

class WiFiReconnectSM {
public:
  typedef void (*BeginFn)(void);          // triggers WiFi.begin(ssid, pass)
  typedef bool (*IsConnectedFn)(void);    // WiFi.status() == WL_CONNECTED
  typedef int  (*GetRssiFn)(void);        // WiFi.RSSI()

  // Called once per RSSI poll (see RSSI_POLL_INTERVAL_MS) with the raw
  // reading. The caller decides what "significant change" means for
  // telemetry purposes -- this class just reports every sample.
  typedef void (*RssiSampleFn)(int rssiDbm, unsigned long nowMs);

  // Called on every state transition, for logging/telemetry.
  typedef void (*StateChangeFn)(WiFiConnState prev, WiFiConnState next,
                                 uint8_t retryAttempt, unsigned long nowMs);

  struct Config {
    unsigned long connectTimeoutMs = 8000;   // give one attempt this long
    unsigned long baseRetryMs = 2000;        // first backoff delay
    unsigned long maxRetryMs = 60000;        // backoff ceiling
    unsigned long rssiPollIntervalMs = 2000; // how often to sample RSSI
  };

  // Convenience overload using default-tuned Config values.
  void begin(BeginFn beginFn, IsConnectedFn isConnectedFn, GetRssiFn rssiFn,
             unsigned long nowMs) {
    Config defaultCfg;
    begin(beginFn, isConnectedFn, rssiFn, nowMs, defaultCfg);
  }

  void begin(BeginFn beginFn, IsConnectedFn isConnectedFn, GetRssiFn rssiFn,
             unsigned long nowMs, Config cfg) {
    this->beginFn = beginFn;
    this->isConnectedFn = isConnectedFn;
    this->rssiFn = rssiFn;
    this->cfg = cfg;
    this->state = WiFiConnState::DISCONNECTED;
    this->retryAttempt = 0;
    this->connectStartMs = nowMs;
    this->waitStartMs = nowMs;
    this->currentRetryDelayMs = cfg.baseRetryMs;
    this->lastRssiPollMs = nowMs;
    this->lastRssi = 0;
  }

  void setRssiSampleCallback(RssiSampleFn fn) { rssiSampleFn = fn; }
  void setStateChangeCallback(StateChangeFn fn) { stateChangeFn = fn; }

  // Call every loop tick. Non-blocking; returns immediately.
  void update(unsigned long nowMs) {
    switch (state) {
      case WiFiConnState::DISCONNECTED:
        beginFn();
        connectStartMs = nowMs;
        transitionTo(WiFiConnState::CONNECTING, nowMs);
        break;

      case WiFiConnState::CONNECTING:
        if (isConnectedFn()) {
          retryAttempt = 0;
          lastRssiPollMs = nowMs;
          lastRssi = rssiFn();
          transitionTo(WiFiConnState::CONNECTED, nowMs);
          if (rssiSampleFn) rssiSampleFn(lastRssi, nowMs);
        } else if (nowMs - connectStartMs >= cfg.connectTimeoutMs) {
          currentRetryDelayMs = computeBackoff(retryAttempt);
          retryAttempt++;
          waitStartMs = nowMs;
          transitionTo(WiFiConnState::WAIT_RETRY, nowMs);
        }
        break;

      case WiFiConnState::CONNECTED:
        if (!isConnectedFn()) {
          transitionTo(WiFiConnState::DISCONNECTED, nowMs);
          break;
        }
        if (nowMs - lastRssiPollMs >= cfg.rssiPollIntervalMs) {
          lastRssiPollMs = nowMs;
          lastRssi = rssiFn();
          if (rssiSampleFn) rssiSampleFn(lastRssi, nowMs);
        }
        break;

      case WiFiConnState::WAIT_RETRY:
        if (nowMs - waitStartMs >= currentRetryDelayMs) {
          transitionTo(WiFiConnState::DISCONNECTED, nowMs);
        }
        break;
    }
  }

  WiFiConnState getState() const { return state; }
  bool isConnected() const { return state == WiFiConnState::CONNECTED; }
  int getLastRssi() const { return lastRssi; }
  uint8_t getRetryAttempt() const { return retryAttempt; }

private:
  BeginFn beginFn = nullptr;
  IsConnectedFn isConnectedFn = nullptr;
  GetRssiFn rssiFn = nullptr;
  RssiSampleFn rssiSampleFn = nullptr;
  StateChangeFn stateChangeFn = nullptr;

  Config cfg;
  WiFiConnState state = WiFiConnState::DISCONNECTED;
  uint8_t retryAttempt = 0;
  unsigned long connectStartMs = 0;
  unsigned long waitStartMs = 0;
  unsigned long currentRetryDelayMs = 0;
  unsigned long lastRssiPollMs = 0;
  int lastRssi = 0;

  void transitionTo(WiFiConnState next, unsigned long nowMs) {
    WiFiConnState prev = state;
    state = next;
    if (stateChangeFn) stateChangeFn(prev, next, retryAttempt, nowMs);
  }

  unsigned long computeBackoff(uint8_t attempt) const {
    // Exponential backoff, capped: base * 2^attempt, clamped to maxRetryMs.
    // Capped at attempt=5 shift to avoid unsigned long overflow on 16-bit
    // platforms with a large baseRetryMs.
    uint8_t shift = attempt > 5 ? 5 : attempt;
    unsigned long delay = cfg.baseRetryMs << shift;
    if (delay > cfg.maxRetryMs || delay < cfg.baseRetryMs /*overflow guard*/) {
      delay = cfg.maxRetryMs;
    }
    return delay;
  }
};

#endif // WIFI_RECONNECT_SM_H
