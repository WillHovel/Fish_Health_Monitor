/*
 * main.c — Aquatic Biomonitor Firmware  (v3.0.0)
 * ================================================
 * Target  : nRF52840 DK / custom PCB (nrf52840dk_nrf52840)
 * RTOS    : Zephyr v3.x
 * SDK     : nRF Connect SDK >= 2.4.0
 *
 * CHANGELOG v3.0.0
 * ----------------
 *   Hardware
 *   • Removed 2-LED + button GPIO (custom PCB has single status LED + AD8232 SDN)
 *   • Added AD8232 SDN control on P0.13 — driven HIGH at boot to enable amplifier
 *   • Status LED on P0.06 (GPIO_ACTIVE_HIGH) — blinks at heart rate while running
 *
 *   DSP / Profiles
 *   • Added 4th species profile: Chromis viridis (HR 100–160 BPM, BPF 1.5–3.0 Hz)
 *   • All filter inits consolidated into dsp_filters_init(), called before threads
 *
 *   BLE GATT Service
 *   • Added Acquisition Control characteristic (UUID …CDEF3): write 0x01=start, 0x00=stop
 *   • Replaces physical button — acquisition now exclusively BLE-commanded
 *
 *   NVS Persistence
 *   • Active species ID saved to flash NVS on every species-switch write
 *   • Loaded at boot — device remembers its last configured species across power cycles
 *
 * PIPELINE OVERVIEW
 * -----------------
 *
 *  [AD8232]─AIN0─▶[ADC Thread 100 Hz]──raw_q──▶[DSP Thread]──telem_q──▶[BLE Thread]──▶ Central
 *  P0.13 SDN=HIGH   k_timer+k_sem         CMSIS Notch+BPF              GATT Notify
 *  (amp enabled)                           Peak Detect                  [BPM,HRV,VR,
 *  P0.06 LED                               BPM/HRV/VR/Cough            Cough,mv,Status,ID]
 *   blinks @ BPM                           Health Classify
 *
 *  BLE Central ──▶ Acquisition Ctrl Char ──▶ k_timer start/stop
 *  BLE Central ──▶ Species Write Char    ──▶ k_mutex ──▶ profile swap + BPF reinit + NVS save
 *
 * THREAD SUMMARY
 * --------------
 *   Thread       | Stack  | Prio | Duty
 *   -------------+--------+------+-------------------------------------------
 *   adc_thread   | 1024 B |  2   | 100 Hz SAADC → raw_sample_queue
 *   dsp_thread   | 3072 B |  3   | Filter + peak + classify → telem_queue
 *   ble_thread   | 2048 B |  5   | Pack telemetry → GATT notify
 *
 * GATT SERVICE LAYOUT (v3)
 * ────────────────────────
 *   Index │ Attribute                    │ UUID suffix │ Properties
 *   ──────┼──────────────────────────────┼─────────────┼──────────────────
 *     [0] │ Primary Service              │ …CDEF0      │
 *     [1] │ Species Write decl.          │ …CDEF1      │ WRITE | WWR
 *     [2] │ Species Write value          │ …CDEF1      │ on_species_write()
 *     [3] │ Telemetry Notify decl.       │ …CDEF2      │ NOTIFY
 *     [4] │ Telemetry Notify value       │ …CDEF2      │ bt_gatt_notify()
 *     [5] │ Telemetry CCC                │             │ R | W
 *     [6] │ Acquisition Control decl.    │ …CDEF3      │ READ | WRITE | WWR
 *     [7] │ Acquisition Control value    │ …CDEF3      │ on_acq_ctrl_write()
 *
 * AUTHOR  : <your-name>
 * DATE    : 2026-10-02
 * LICENSE : Apache-2.0
 */

/* ═══════════════════════════════════════════════════════════════════════════
 * INCLUDES
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include <arm_math.h>   /* CMSIS-DSP via CONFIG_CMSIS_DSP=y */
#include <string.h>
#include <math.h>       /* sqrtf() via CONFIG_NEWLIB_LIBC=y  */

/* ═══════════════════════════════════════════════════════════════════════════
 * LOGGING
 * ═══════════════════════════════════════════════════════════════════════════ */
LOG_MODULE_REGISTER(aquabio, CONFIG_LOG_DEFAULT_LEVEL);

/* ═══════════════════════════════════════════════════════════════════════════
 * DEVICETREE ALIASES
 *
 * Resolved at compile time from boards/nrf52840dk_nrf52840.overlay (v3).
 * Pin changes only require editing the overlay — not this file.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define LED_STATUS_ALIAS  DT_ALIAS(led_status)          /* P0.06, ACTIVE_HIGH */
#define ZEPHYR_USER_NODE  DT_PATH(zephyr_user)          /* AD8232 SDN on P0.13 */

/* ═══════════════════════════════════════════════════════════════════════════
 * SAMPLING PARAMETERS
 * ═══════════════════════════════════════════════════════════════════════════ */
#define SAMPLE_RATE_HZ       100U
#define SAMPLE_PERIOD_MS     (1000U / SAMPLE_RATE_HZ)  /* 10 ms */
#define ADC_VREF_MV          3600U
#define ADC_RESOLUTION_BITS  12U
#define ADC_FULL_SCALE       (1U << ADC_RESOLUTION_BITS)  /* 4096 */

/* ═══════════════════════════════════════════════════════════════════════════
 * PEAK DETECTOR TUNING CONSTANTS
 * ═══════════════════════════════════════════════════════════════════════════ */
#define PEAK_WINDOW_LEN   200U   /* 2 s at 100 Hz — Welford statistics window */
#define PEAK_ALPHA        2.5f   /* primary threshold = µ + PEAK_ALPHA * σ    */
#define COUGH_ALPHA       4.5f   /* cough threshold   = µ + COUGH_ALPHA * σ   */
#define MIN_RR_MS         250U   /* 250 ms refractory ≡ 240 BPM absolute max  */
#define HRV_WINDOW        8U     /* RR intervals averaged for RMSSD            */
#define COUGH_WINDOW_SEC  60U    /* rolling window for coughs-per-minute       */

/* ═══════════════════════════════════════════════════════════════════════════
 * NVS STORAGE
 *
 * The NVS filesystem is mounted on the "storage" flash partition defined in
 * the board overlay.  We persist a single uint8 — the active species ID —
 * under NVS key ID 1.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define NVS_SPECIES_KEY   1U    /* NVS entry ID for the species ID byte */

static struct nvs_fs  nvs_storage;
static bool           nvs_ready = false;   /* set to true after nvs_mount()  */

/* ═══════════════════════════════════════════════════════════════════════════
 * SPECIES PROFILE DEFINITIONS
 * ═══════════════════════════════════════════════════════════════════════════ */
#define BPF_COEFF_LEN   (5U * 2U)   /* 2 biquad stages × 5 coefficients */

/**
 * @brief Per-species physiological profile.
 *
 * Contains both the physiological reference bounds used by the health
 * classifier and the CMSIS-DSP DF2T coefficients for the species-tuned
 * bandpass filter.  Stored in flash (const) — only a pointer to the active
 * profile is kept in RAM.
 *
 * Run tools/generate_coeffs.py to regenerate correct bpf_coeffs[] values.
 */
typedef struct {
    char      name[40];              /* human-readable name (logging)         */
    uint8_t   id;                    /* species ID written via BLE (1–4)      */

    /* Cardiac */
    float32_t min_bpm;
    float32_t max_bpm;

    /* Respiratory */
    float32_t min_vr;                /* breaths per minute */
    float32_t max_vr;

    /* HRV */
    float32_t min_hrv_rmssd;         /* ms — below = autonomic stress flag   */

    /* Cough / artefact */
    float32_t max_coughs_per_min;

    /* DSP — 2-stage Butterworth BPF coefficients in CMSIS DF2T format */
    float32_t bpf_coeffs[BPF_COEFF_LEN];

    /* Physiology */
    float32_t ecg_vr_ratio;          /* cardiac beats per ventilation cycle  */
} fish_profile_t;

/* ─────────────────────────────────────────────────────────────────────────
 * PROFILE 1: Glass Catfish  (Kryptopterus vitreolus)
 * BPF: 0.5–1.5 Hz (slow teleost HR ~0.5–1.2 Hz).
 * Run: python tools/generate_coeffs.py  to regenerate bpf_coeffs.
 * ─────────────────────────────────────────────────────────────────────────*/
static const fish_profile_t PROFILE_GLASS_CATFISH = {
    .name               = "Glass Catfish (K. vitreolus)",
    .id                 = 1U,
    .min_bpm            = 30.0f,
    .max_bpm            = 70.0f,
    .min_vr             = 40.0f,
    .max_vr             = 60.0f,
    .min_hrv_rmssd      = 10.0f,
    .max_coughs_per_min = 5.0f,
    .ecg_vr_ratio       = 2.0f,
    /* BPF 0.5–1.5 Hz | Butterworth 2nd-order | fs=100 Hz  *** PLACEHOLDER *** */
    .bpf_coeffs = {
        /* Stage 0:  b0          b1         b2         -a1         -a2       */
         0.00482f,  0.00000f, -0.00482f,   1.97843f, -0.98068f,
        /* Stage 1:  b0          b1         b2         -a1         -a2       */
         1.00000f, -2.00000f,  1.00000f,   1.99311f, -0.99390f,
    }
};

/* ─────────────────────────────────────────────────────────────────────────
 * PROFILE 2: Atlantic Silverside  (Menidia menidia)
 * BPF: 1.3–2.5 Hz (fast pelagic teleost).
 * ─────────────────────────────────────────────────────────────────────────*/
static const fish_profile_t PROFILE_MENIDIA_MENIDIA = {
    .name               = "Atlantic Silverside (M. menidia)",
    .id                 = 2U,
    .min_bpm            = 80.0f,
    .max_bpm            = 120.0f,
    .min_vr             = 60.0f,
    .max_vr             = 100.0f,
    .min_hrv_rmssd      = 5.0f,
    .max_coughs_per_min = 10.0f,
    .ecg_vr_ratio       = 1.5f,
    /* BPF 1.3–2.5 Hz | Butterworth 2nd-order | fs=100 Hz  *** PLACEHOLDER *** */
    .bpf_coeffs = {
         0.01172f,  0.00000f, -0.01172f,   1.95441f, -0.97704f,
         1.00000f, -2.00000f,  1.00000f,   1.98287f, -0.98373f,
    }
};

/* ─────────────────────────────────────────────────────────────────────────
 * PROFILE 3: Inland Silverside  (Menidia beryllina)
 * Euryhaline; physiologically similar to M. menidia.
 * ─────────────────────────────────────────────────────────────────────────*/
static const fish_profile_t PROFILE_MENIDIA_BERYLLINA = {
    .name               = "Inland Silverside (M. beryllina)",
    .id                 = 3U,
    .min_bpm            = 80.0f,
    .max_bpm            = 120.0f,
    .min_vr             = 60.0f,
    .max_vr             = 100.0f,
    .min_hrv_rmssd      = 5.0f,
    .max_coughs_per_min = 10.0f,
    .ecg_vr_ratio       = 1.5f,
    /* BPF 1.3–2.5 Hz | fs=100 Hz  *** PLACEHOLDER *** */
    .bpf_coeffs = {
         0.01172f,  0.00000f, -0.01172f,   1.95441f, -0.97704f,
         1.00000f, -2.00000f,  1.00000f,   1.98287f, -0.98373f,
    }
};

/* ─────────────────────────────────────────────────────────────────────────
 * PROFILE 4: Chromis viridis  (Blue-green Chromis)  ← NEW in v3
 *
 * A small Indo-Pacific damselffish with distinctly higher cardiac output
 * than the Menidia spp.  HR range 100–160 BPM at 26°C.  BPF tuned to
 * 1.5–3.0 Hz to encompass the faster ventricular-rate fundamental.
 * Ventilation rate 80–130 breaths/min at rest; cough limit 12/min.
 * ─────────────────────────────────────────────────────────────────────────*/
static const fish_profile_t PROFILE_CHROMIS_VIRIDIS = {
    .name               = "Chromis viridis (Blue-green Chromis)",
    .id                 = 4U,
    .min_bpm            = 100.0f,
    .max_bpm            = 160.0f,
    .min_vr             = 80.0f,
    .max_vr             = 130.0f,
    .min_hrv_rmssd      = 3.0f,    /* ms — tighter bound for the faster rate  */
    .max_coughs_per_min = 12.0f,
    .ecg_vr_ratio       = 1.3f,
    /* BPF 1.5–3.0 Hz | Butterworth 2nd-order | fs=100 Hz  *** PLACEHOLDER *** */
    .bpf_coeffs = {
        /* Stage 0:  b0          b1         b2         -a1         -a2       */
         0.02008f,  0.00000f, -0.02008f,   1.93857f, -0.95984f,
        /* Stage 1:  b0          b1         b2         -a1         -a2       */
         1.00000f, -2.00000f,  1.00000f,   1.97581f, -0.97656f,
    }
};

/** 1-based lookup table — index 0 unused.  Add new species here. */
#define NUM_SPECIES  4U

static const fish_profile_t *const profile_table[NUM_SPECIES + 1U] = {
    NULL,                         /* [0] — no species ID 0  */
    &PROFILE_GLASS_CATFISH,       /* [1]                    */
    &PROFILE_MENIDIA_MENIDIA,     /* [2]                    */
    &PROFILE_MENIDIA_BERYLLINA,   /* [3]                    */
    &PROFILE_CHROMIS_VIRIDIS,     /* [4]                    */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * ACTIVE PROFILE — mutex-protected shared state
 *
 * profile_mutex must be held when reading or writing active_profile or
 * when calling bpf_reinit_locked().  k_mutex uses priority inheritance so
 * the BLE RX callback never experiences unbounded priority inversion waiting
 * for the DSP thread to release the lock.
 * ═══════════════════════════════════════════════════════════════════════════ */
static K_MUTEX_DEFINE(profile_mutex);
static const fish_profile_t *active_profile = &PROFILE_GLASS_CATFISH;

/* ═══════════════════════════════════════════════════════════════════════════
 * HEALTH STATE ENUM
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef enum {
    STATUS_NORMAL               = 0,
    STATUS_TACHYCARDIA          = 1,
    STATUS_BRADYCARDIA          = 2,
    STATUS_RESPIRATORY_DISTRESS = 3,
    STATUS_LOW_HRV              = 4,
    STATUS_NO_SIGNAL            = 5,
} health_state_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * TELEMETRY PACKET
 *
 * 26-byte, packed, little-endian.  Matches the struct.unpack format
 * '<5fBB4x' used by tools/ble_monitor.py.
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    float32_t bpm;
    float32_t hrv_rmssd;
    float32_t vr;
    float32_t cough_rate;
    float32_t filtered_mv;
    uint8_t   health_state;
    uint8_t   species_id;
    uint8_t   reserved[4];
} __attribute__((packed)) telemetry_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * MESSAGE QUEUES
 * ═══════════════════════════════════════════════════════════════════════════ */
#define MSGQ_DEPTH  8U

typedef int16_t raw_sample_t;

K_MSGQ_DEFINE(raw_sample_queue, sizeof(raw_sample_t), MSGQ_DEPTH, 4);
K_MSGQ_DEFINE(telem_queue,      sizeof(telemetry_t),  4U,         4);

/* ═══════════════════════════════════════════════════════════════════════════
 * THREAD DEFINITIONS
 * ═══════════════════════════════════════════════════════════════════════════ */
#define ADC_THREAD_STACK_SIZE   1024
#define DSP_THREAD_STACK_SIZE   3072
#define BLE_THREAD_STACK_SIZE   2048

#define ADC_THREAD_PRIORITY     2
#define DSP_THREAD_PRIORITY     3
#define BLE_THREAD_PRIORITY     5

K_THREAD_STACK_DEFINE(adc_stack, ADC_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(dsp_stack, DSP_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(ble_stack, BLE_THREAD_STACK_SIZE);

static struct k_thread adc_thread_data;
static struct k_thread dsp_thread_data;
static struct k_thread ble_thread_data;

/* ═══════════════════════════════════════════════════════════════════════════
 * GPIO DEVICE HANDLES
 *
 * v3 hardware: single status LED (P0.06) + AD8232 SDN control (P0.13).
 * No physical buttons — acquisition is BLE-commanded.
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Status LED — P0.06, GPIO_ACTIVE_HIGH.  Blinks at detected heart rate. */
static const struct gpio_dt_spec led_status =
    GPIO_DT_SPEC_GET(LED_STATUS_ALIAS, gpios);

/**
 * AD8232 SDN (shutdown) control — P0.13, GPIO_ACTIVE_HIGH.
 * gpio_pin_set(..., 1) drives P0.13 HIGH → SDN HIGH → amplifier ON.
 * gpio_pin_set(..., 0) drives P0.13 LOW  → SDN LOW  → amplifier shutdown.
 * Must be driven HIGH before the first SAADC conversion.
 */
static const struct gpio_dt_spec ad8232_sdn =
    GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, sdn_gpios);
/* ═══════════════════════════════════════════════════════════════════════════
 * ADC CHANNEL CONFIGURATION
 * ═══════════════════════════════════════════════════════════════════════════ */
static const struct adc_dt_spec adc_channel =
    ADC_DT_SPEC_GET(DT_PATH(zephyr_user));
static int16_t          adc_raw_buf[1];
static struct adc_sequence adc_seq = {
    .buffer      = adc_raw_buf,
    .buffer_size = sizeof(adc_raw_buf),
};

/* ═══════════════════════════════════════════════════════════════════════════
 * CMSIS-DSP FILTER INSTANCES
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ─── Notch (all species, coefficients never change) ────────────────────── */
#define NOTCH_NUM_STAGES  1U

static float32_t notch_coeffs[5 * NOTCH_NUM_STAGES] = {
    /* b0          b1          b2         -a1         -a2
     * Notch: f0=25 Hz, Q=12, fs=100 Hz   *** PLACEHOLDER *** */
     0.91207f, -1.40680f,  0.91207f,   1.40680f,  -0.82414f
};
static float32_t notch_state[2 * NOTCH_NUM_STAGES];
static arm_biquad_cascade_df2T_instance_f32 notch_inst;

/* ─── Species bandpass (re-initialised under profile_mutex on species swap) */
#define BPF_NUM_STAGES    2U

static float32_t bpf_state[2 * BPF_NUM_STAGES];
static arm_biquad_cascade_df2T_instance_f32 bpf_inst;

/**
 * @brief Re-initialise the species BPF with a new profile's coefficients.
 *
 * MUST be called with profile_mutex held.
 * Zeroes the delay-line state to suppress transient artefacts.
 *
 * @param profile  New active species profile.
 */
static void bpf_reinit_locked(const fish_profile_t *profile)
{
    memset(bpf_state, 0, sizeof(bpf_state));
    arm_biquad_cascade_df2T_init_f32(
        &bpf_inst,
        BPF_NUM_STAGES,
        (float32_t *)profile->bpf_coeffs,   /* CMSIS reads, never writes  */
        bpf_state
    );
    LOG_INF("BPF reinit → %s", profile->name);
}

/**
 * @brief Initialise both DSP filters.
 *
 * Called once from main() after nvs_init_and_load() has set active_profile,
 * and BEFORE any threads are spawned.  This avoids an init race in the DSP
 * thread.
 */
static void dsp_filters_init(void)
{
    /* Notch — independent of species */
    memset(notch_state, 0, sizeof(notch_state));
    arm_biquad_cascade_df2T_init_f32(&notch_inst, NOTCH_NUM_STAGES,
                                     notch_coeffs, notch_state);

    /* Species BPF — uses the profile already loaded from NVS */
    k_mutex_lock(&profile_mutex, K_FOREVER);
    bpf_reinit_locked(active_profile);
    k_mutex_unlock(&profile_mutex);

    LOG_INF("DSP filters initialised (notch + %s BPF)", active_profile->name);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * NVS — Non-Volatile Storage for species ID
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Mount NVS and restore the last saved species ID.
 *
 * The storage_partition flash node is defined in the board overlay.
 * NVS uses a cyclic write scheme so each write wears one sector evenly.
 *
 * @return 0 on success, negative errno if NVS mount fails.
 */
static int nvs_init_and_load(void)
{
    const struct device *flash_dev = FIXED_PARTITION_DEVICE(storage_partition);

    if (!device_is_ready(flash_dev)) {
        LOG_ERR("NVS: flash device not ready");
        return -ENODEV;
    }

    struct flash_pages_info page_info;
    nvs_storage.flash_device = flash_dev;
    nvs_storage.offset        = FIXED_PARTITION_OFFSET(storage_partition);

    int ret = flash_get_page_info_by_offs(flash_dev, nvs_storage.offset, &page_info);
    if (ret < 0) {
        LOG_ERR("NVS: flash_get_page_info_by_offs failed (err %d)", ret);
        return ret;
    }

    /*
     * sector_size:  size of one flash erase page (typically 4096 bytes on nRF52840)
     * sector_count: number of pages reserved for NVS.  3 pages = 12 kB gives
     *               >100 000 write cycles for a single 1-byte entry before wear.
     */
    nvs_storage.sector_size  = page_info.size;
    nvs_storage.sector_count = 3U;

    ret = nvs_mount(&nvs_storage);
    if (ret < 0) {
        LOG_ERR("NVS: mount failed (err %d)", ret);
        return ret;
    }
    nvs_ready = true;
    LOG_INF("NVS: mounted (sector_size=%u, sectors=%u)",
            nvs_storage.sector_size, nvs_storage.sector_count);

    /* Try to read the previously saved species ID. */
    uint8_t saved_id = 0U;
    ssize_t rc = nvs_read(&nvs_storage, NVS_SPECIES_KEY, &saved_id, sizeof(saved_id));

    if (rc > 0 && saved_id >= 1U && saved_id <= NUM_SPECIES) {
        /* Valid saved ID found — apply it. */
        k_mutex_lock(&profile_mutex, K_FOREVER);
        active_profile = profile_table[saved_id];
        /* Note: bpf_reinit_locked is called by dsp_filters_init() after this */
        k_mutex_unlock(&profile_mutex);
        LOG_INF("NVS: restored species ID=%u (%s)", saved_id, active_profile->name);
    } else {
        /* First boot or corrupted entry — write the default and continue. */
        nvs_write(&nvs_storage, NVS_SPECIES_KEY,
                  &active_profile->id, sizeof(uint8_t));
        LOG_INF("NVS: no saved species — wrote default ID=%u", active_profile->id);
    }

    return 0;
}

/**
 * @brief Persist the current species ID to NVS.
 *
 * Called from on_species_write() after a successful profile swap.
 * nvs_write() is idempotent: if the value is unchanged it skips the erase.
 *
 * @param species_id  Species ID to save (1–NUM_SPECIES).
 */
static void nvs_save_species(uint8_t species_id)
{
    if (!nvs_ready) {
        LOG_WRN("NVS not ready — species ID not persisted");
        return;
    }

    ssize_t rc = nvs_write(&nvs_storage, NVS_SPECIES_KEY,
                            &species_id, sizeof(species_id));
    if (rc < 0) {
        LOG_WRN("NVS: write failed (err %zd)", rc);
    } else {
        LOG_INF("NVS: species ID=%u saved", species_id);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * BLE SERVICE UUID DEFINITIONS
 *
 * Service UUID         : 12345678-1234-5678-1234-56789ABCDEF0
 * Species Write UUID   : 12345678-1234-5678-1234-56789ABCDEF1
 * Telemetry Notify UUID: 12345678-1234-5678-1234-56789ABCDEF2
 * Acq. Control UUID    : 12345678-1234-5678-1234-56789ABCDEF3  ← v3 new
 * ═══════════════════════════════════════════════════════════════════════════ */
#define BT_UUID_AQUABIO_SVC_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789ABCDEF0)
#define BT_UUID_AQUABIO_SPECIES_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789ABCDEF1)
#define BT_UUID_AQUABIO_TELEM_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789ABCDEF2)
#define BT_UUID_AQUABIO_ACQCTRL_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789ABCDEF3)

static struct bt_uuid_128 aquabio_svc_uuid     = BT_UUID_INIT_128(BT_UUID_AQUABIO_SVC_VAL);
static struct bt_uuid_128 aquabio_species_uuid = BT_UUID_INIT_128(BT_UUID_AQUABIO_SPECIES_VAL);
static struct bt_uuid_128 aquabio_telem_uuid   = BT_UUID_INIT_128(BT_UUID_AQUABIO_TELEM_VAL);
static struct bt_uuid_128 aquabio_acqctrl_uuid = BT_UUID_INIT_128(BT_UUID_AQUABIO_ACQCTRL_VAL);

/* ═══════════════════════════════════════════════════════════════════════════
 * ACQUISITION STATE
 *
 * Controlled by the Acquisition Control GATT characteristic.
 * The k_timer is started/stopped from the GATT write callback (BLE RX
 * thread context) — k_timer_start/stop are ISR-safe in Zephyr.
 *
 * The atomic flag acq_running mirrors timer state so the BLE thread can
 * report the current state in the ACQ_CTRL read callback without locking.
 * ═══════════════════════════════════════════════════════════════════════════ */
static K_SEM_DEFINE(adc_sem, 0, 1);

static void adc_timer_expiry(struct k_timer *timer_id)
{
    ARG_UNUSED(timer_id);
    k_sem_give(&adc_sem);
}

static K_TIMER_DEFINE(adc_timer, adc_timer_expiry, NULL);

/** 1 = acquisition running, 0 = stopped.  Written in BLE callback context. */
static atomic_t acq_running = ATOMIC_INIT(0);

/* ═══════════════════════════════════════════════════════════════════════════
 * BLE STATE
 * ═══════════════════════════════════════════════════════════════════════════ */
static struct bt_conn *current_conn;
static bool            notify_enabled;

/* ═══════════════════════════════════════════════════════════════════════════
 * GATT CHARACTERISTIC CALLBACKS
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Telemetry CCC descriptor changed callback.
 *
 * Invoked when the central subscribes (0x0001) or unsubscribes (0x0000)
 * from telemetry notifications.
 */
static void telem_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Telemetry notifications %s", notify_enabled ? "ENABLED" : "DISABLED");
}

/**
 * @brief Species Write characteristic write callback.
 *
 * Accepts a 1-byte species ID (1–NUM_SPECIES), hot-swaps the active profile
 * under profile_mutex, re-initialises the BPF filter, and saves the new ID
 * to NVS so it persists across reboots.
 *
 * THREAD SAFETY:
 *   Runs in BLE RX thread.  The DSP thread is blocked for < 2 µs (the
 *   bpf_reinit_locked call) while the mutex is held.
 */
static ssize_t on_species_write(struct bt_conn *conn,
                                const struct bt_gatt_attr *attr,
                                const void *buf,
                                uint16_t len,
                                uint16_t offset,
                                uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset != 0U) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len != 1U) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    uint8_t requested_id = ((const uint8_t *)buf)[0];

    if (requested_id < 1U || requested_id > NUM_SPECIES) {
        LOG_WRN("Species write: invalid ID %u (valid 1–%u)", requested_id, NUM_SPECIES);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    const fish_profile_t *new_profile = profile_table[requested_id];

    /* ── Hot-swap under mutex ── */
    k_mutex_lock(&profile_mutex, K_FOREVER);

    if (active_profile->id == requested_id) {
        k_mutex_unlock(&profile_mutex);
        LOG_INF("Species write: ID=%u already active (%s)", requested_id, active_profile->name);
        return (ssize_t)len;
    }

    active_profile = new_profile;
    bpf_reinit_locked(active_profile);

    k_mutex_unlock(&profile_mutex);

    /* ── Persist to NVS ── */
    nvs_save_species(requested_id);

    LOG_INF("Species → ID=%u (%s)", requested_id, new_profile->name);
    return (ssize_t)len;
}

/**
 * @brief Acquisition Control characteristic write callback.
 *
 * The central writes a single byte to start or stop acquisition:
 *   0x01 → start  (starts the 100 Hz k_timer)
 *   0x00 → stop   (stops the k_timer; drains queues on next DSP/BLE wakeup)
 *
 * k_timer_start/stop are ISR-safe so can be called from BLE RX context.
 */
static ssize_t on_acq_ctrl_write(struct bt_conn *conn,
                                  const struct bt_gatt_attr *attr,
                                  const void *buf,
                                  uint16_t len,
                                  uint16_t offset,
                                  uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset != 0U || len != 1U) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    uint8_t cmd = ((const uint8_t *)buf)[0];

    if (cmd == 0x01U) {
        /* ── START ── */
        if (!atomic_cas(&acq_running, 0, 1)) {
            LOG_INF("Acq: already running");
            return (ssize_t)len;
        }
        k_timer_start(&adc_timer,
                      K_MSEC(SAMPLE_PERIOD_MS),
                      K_MSEC(SAMPLE_PERIOD_MS));
        gpio_pin_set_dt(&led_status, 1);   /* LED on = connected + running */
        LOG_INF("Acquisition STARTED via BLE");

    } else if (cmd == 0x00U) {
        /* ── STOP ── */
        if (!atomic_cas(&acq_running, 1, 0)) {
            LOG_INF("Acq: already stopped");
            return (ssize_t)len;
        }
        k_timer_stop(&adc_timer);
        gpio_pin_set_dt(&led_status, 0);   /* LED off = stopped */
        LOG_INF("Acquisition STOPPED via BLE");

    } else {
        LOG_WRN("Acq ctrl: unknown command 0x%02x", cmd);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    return (ssize_t)len;
}

/**
 * @brief Acquisition Control characteristic read callback.
 *
 * Allows the central to query the current running state on connect:
 *   0x01 → running, 0x00 → stopped.
 */
static ssize_t on_acq_ctrl_read(struct bt_conn *conn,
                                 const struct bt_gatt_attr *attr,
                                 void *buf,
                                 uint16_t len,
                                 uint16_t offset)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);

    uint8_t state = (uint8_t)atomic_get(&acq_running);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &state, sizeof(state));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * GATT SERVICE TABLE  (v3 layout — 8 attributes)
 *
 *   [0]   Primary Service
 *   [1,2] Species Write
 *   [3,4] Telemetry Notify
 *   [5]   Telemetry CCC
 *   [6,7] Acquisition Control  ← NEW
 * ═══════════════════════════════════════════════════════════════════════════ */
BT_GATT_SERVICE_DEFINE(aquabio_svc,

    BT_GATT_PRIMARY_SERVICE(&aquabio_svc_uuid),

    /* [1,2] Species Write — accepts 1-byte species ID */
    BT_GATT_CHARACTERISTIC(
        &aquabio_species_uuid.uuid,
        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE,
        NULL,
        on_species_write,
        NULL
    ),

    /* [3,4] Telemetry Notify — 26-byte packets at detected peak rate */
    BT_GATT_CHARACTERISTIC(
        &aquabio_telem_uuid.uuid,
        BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_NONE,
        NULL, NULL, NULL
    ),

    /* [5] CCC descriptor for telemetry */
    BT_GATT_CCC(telem_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* [6,7] Acquisition Control — 0x00 stop, 0x01 start */
    BT_GATT_CHARACTERISTIC(
        &aquabio_acqctrl_uuid.uuid,
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
        on_acq_ctrl_read,
        on_acq_ctrl_write,
        NULL
    ),
);

/* ═══════════════════════════════════════════════════════════════════════════
 * BLE ADVERTISING DATA
 * ═══════════════════════════════════════════════════════════════════════════ */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE,
            CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_AQUABIO_SVC_VAL),
};

/* ═══════════════════════════════════════════════════════════════════════════
 * BLE CONNECTION CALLBACKS
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("BLE connection failed (err %u)", err);
        return;
    }
    LOG_INF("BLE connected");
    current_conn = bt_conn_ref(conn);
    /* LED on solid until acquisition start command sets it to blink mode */
    gpio_pin_set_dt(&led_status, 1);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("BLE disconnected (reason 0x%02x)", reason);

    /* Stop acquisition if running — safe to call even if already stopped */
    if (atomic_cas(&acq_running, 1, 0)) {
        k_timer_stop(&adc_timer);
        LOG_INF("Acquisition halted on disconnect");
    }

    bt_conn_unref(current_conn);
    current_conn   = NULL;
    notify_enabled = false;
    gpio_pin_set_dt(&led_status, 0);

    int ret = bt_le_adv_start(
        BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,
                        BT_GAP_ADV_FAST_INT_MIN_2,
                        BT_GAP_ADV_FAST_INT_MAX_2,
                        NULL),
        ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (ret) {
        LOG_ERR("Restart advertising failed (err %d)", ret);
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected    = on_connected,
    .disconnected = on_disconnected,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * ADC THREAD
 *
 * Waits on adc_sem (posted by k_timer ISR every 10 ms = 100 Hz).
 * When the timer is stopped (acquisition halted), the semaphore is never
 * posted so the thread simply blocks indefinitely — zero CPU cost.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void adc_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    const struct device *adc_dev = adc_channel.dev;

    int ret = adc_channel_setup_dt(&adc_channel);
    if (ret < 0) { LOG_ERR("ADC setup failed (err %d)", ret); return; }

    ret = adc_sequence_init_dt(&adc_channel, &adc_seq);
    if (ret < 0) { LOG_ERR("ADC seq init failed (err %d)", ret); return; }

    LOG_INF("ADC thread ready — waiting for acquisition start command");

    while (true) {
        /* Blocks until k_timer fires (controlled by BLE acquisition-ctrl char) */
        k_sem_take(&adc_sem, K_FOREVER);

        ret = adc_read(adc_dev, &adc_seq);
        if (ret < 0) {
            LOG_WRN("ADC read error (err %d) — skipping", ret);
            continue;
        }

        raw_sample_t raw = (raw_sample_t)adc_raw_buf[0];

        ret = k_msgq_put(&raw_sample_queue, &raw, K_NO_WAIT);
        if (ret != 0) {
            LOG_WRN("raw_sample_queue full — sample dropped");
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DSP THREAD — FILTER → PEAK DETECT → HEALTH CLASSIFY → TELEMETRY
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ─── Welford sliding-window statistics ──────────────────────────────────── */
static float32_t peak_window[PEAK_WINDOW_LEN];
static uint32_t  peak_window_idx  = 0U;
static uint32_t  peak_window_fill = 0U;
static float32_t welford_mean = 0.0f;
static float32_t welford_M2   = 0.0f;
static uint32_t  welford_n    = 0U;

/**
 * @brief Welford online mean/variance with fixed-length window downdate.
 *
 * When the window is full, the oldest sample is removed via a reverse
 * Welford step before the new sample is incorporated.  This maintains
 * a strictly PEAK_WINDOW_LEN-sample window in O(1) per call.
 *
 * @param x        New filtered sample (mV).
 * @param mean_out Output mean over the window.
 * @param std_out  Output standard deviation over the window.
 */
static void welford_update(float32_t x,
                           float32_t *mean_out,
                           float32_t *std_out)
{
    if (peak_window_fill == PEAK_WINDOW_LEN) {
        /* Remove oldest sample (downdate) */
        float32_t x_old  = peak_window[peak_window_idx];
        float32_t m_old  = welford_mean;
        welford_mean = (welford_mean * (float32_t)welford_n - x_old)
                     / (float32_t)(welford_n - 1U);
        welford_M2  -= (x_old - m_old) * (x_old - welford_mean);
        if (welford_M2 < 0.0f) { welford_M2 = 0.0f; }   /* numerical guard */
        welford_n--;
    }

    /* Add new sample (update) */
    welford_n++;
    float32_t delta  = x - welford_mean;
    welford_mean    += delta / (float32_t)welford_n;
    welford_M2      += delta * (x - welford_mean);

    /* Store in circular buffer */
    peak_window[peak_window_idx] = x;
    peak_window_idx  = (peak_window_idx + 1U) % PEAK_WINDOW_LEN;
    if (peak_window_fill < PEAK_WINDOW_LEN) { peak_window_fill++; }

    *mean_out = welford_mean;
    *std_out  = (welford_n > 1U)
                ? sqrtf(welford_M2 / (float32_t)(welford_n - 1U))
                : 0.0f;
}

/* ─── R-R interval / BPM / HRV ──────────────────────────────────────────── */
static int64_t  last_peak_ms  = 0;
static float32_t live_bpm     = 0.0f;
static float32_t live_hrv     = 0.0f;
static float32_t live_vr      = 0.0f;

static float32_t rr_buf[HRV_WINDOW];
static uint8_t   rr_idx  = 0U;
static uint8_t   rr_fill = 0U;

/**
 * @brief Update the RR buffer and recompute RMSSD.
 *
 *   RMSSD = sqrt( (1/(N-1)) * Σ (RR[i] - RR[i-1])^2 )
 *
 * @param rr_ms  New R-R interval in milliseconds.
 */
static void hrv_update(float32_t rr_ms)
{
    rr_buf[rr_idx] = rr_ms;
    rr_idx  = (rr_idx + 1U) % HRV_WINDOW;
    if (rr_fill < HRV_WINDOW) { rr_fill++; }

    if (rr_fill < 2U) { live_hrv = 0.0f; return; }

    float32_t sum_sq = 0.0f;
    uint8_t   n      = (rr_fill < HRV_WINDOW) ? rr_fill : HRV_WINDOW;

    for (uint8_t i = 1U; i < n; i++) {
        uint8_t ic  = (rr_idx + HRV_WINDOW - i)      % HRV_WINDOW;
        uint8_t ip  = (rr_idx + HRV_WINDOW - i - 1U) % HRV_WINDOW;
        float32_t d = rr_buf[ic] - rr_buf[ip];
        sum_sq += d * d;
    }
    live_hrv = sqrtf(sum_sq / (float32_t)(n - 1U));
}

/* ─── Cough detection ────────────────────────────────────────────────────── */
#define COUGH_BUF_LEN  32U

static int64_t  cough_ts[COUGH_BUF_LEN];
static uint8_t  cough_idx  = 0U;
static uint32_t cough_fill = 0U;
static float32_t live_cough = 0.0f;

static void cough_record(int64_t now_ms)
{
    cough_ts[cough_idx]  = now_ms;
    cough_idx  = (cough_idx + 1U) % COUGH_BUF_LEN;
    if (cough_fill < COUGH_BUF_LEN) { cough_fill++; }

    int64_t  window_start = now_ms - (int64_t)(COUGH_WINDOW_SEC * 1000U);
    uint32_t count = 0U;
    uint32_t n = (cough_fill < COUGH_BUF_LEN) ? cough_fill : COUGH_BUF_LEN;
    for (uint32_t i = 0U; i < n; i++) {
        if (cough_ts[i] >= window_start) { count++; }
    }
    live_cough = (float32_t)count;
}

/* ─── Health classifier ──────────────────────────────────────────────────── */

/**
 * @brief Compare live metrics to the active profile's physiological bounds.
 *
 * MUST be called with profile_mutex held.
 *
 * @param now_ms  Current k_uptime_get() value.
 * @return        Current health_state_t classification.
 */
static health_state_t classify_health(int64_t now_ms)
{
    /* No-signal guard: > 5 s since last confirmed peak */
    if (last_peak_ms == 0 || (now_ms - last_peak_ms) > 5000LL) {
        return STATUS_NO_SIGNAL;
    }

    if (live_bpm > active_profile->max_bpm)            { return STATUS_TACHYCARDIA;          }
    if (live_bpm < active_profile->min_bpm)            { return STATUS_BRADYCARDIA;           }

    bool vr_bad    = (live_vr < active_profile->min_vr) || (live_vr > active_profile->max_vr);
    bool cough_bad = (live_cough > active_profile->max_coughs_per_min);
    if (vr_bad || cough_bad)                           { return STATUS_RESPIRATORY_DISTRESS;  }

    if (rr_fill >= 2U && live_hrv < active_profile->min_hrv_rmssd) {
        return STATUS_LOW_HRV;
    }

    return STATUS_NORMAL;
}

/**
 * @brief DSP processing thread.
 *
 * Execution per sample:
 *   1. k_msgq_get() — block until raw ADC sample arrives
 *   2. ADC code → mV
 *   3. Notch filter (no lock)
 *   4. Species BPF (lock held for < 1 µs)
 *   5. Welford window update
 *   6. Threshold comparison — peak / cough detection
 *   7. On confirmed peak:
 *        RR interval → BPM, HRV, VR
 *        Cough record if above T_cough
 *        Health classification
 *        LED toggle (visual BPM indicator)
 *        Telemetry packet → telem_queue
 */
static void dsp_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    LOG_INF("DSP thread ready");

    bool in_peak = false;

    float32_t sample_in;
    float32_t after_notch;
    float32_t after_bpf;

    while (true) {
        raw_sample_t raw;
        k_msgq_get(&raw_sample_queue, &raw, K_FOREVER);

        /* ── 1. ADC code → millivolts ── */
        sample_in = ((float32_t)raw * (float32_t)ADC_VREF_MV)
                  / (float32_t)ADC_FULL_SCALE;

        /* ── 2. Notch filter ── */
        arm_biquad_cascade_df2T_f32(&notch_inst, &sample_in, &after_notch, 1U);

        /* ── 3. Species BPF (held for < 1 µs) ── */
        k_mutex_lock(&profile_mutex, K_FOREVER);
        arm_biquad_cascade_df2T_f32(&bpf_inst, &after_notch, &after_bpf, 1U);
        const fish_profile_t *prof = active_profile;   /* local snapshot     */
        k_mutex_unlock(&profile_mutex);

        /* ── 4. Welford update ── */
        float32_t win_mean, win_std;
        welford_update(after_bpf, &win_mean, &win_std);

        /* Wait for half a window before thresholds are meaningful */
        if (peak_window_fill < (PEAK_WINDOW_LEN / 2U)) {
            continue;
        }

        float32_t t_peak  = win_mean + PEAK_ALPHA  * win_std;
        float32_t t_cough = win_mean + COUGH_ALPHA * win_std;

        /* ── 5. Peak detection (rising-edge + refractory guard) ── */
        int64_t now_ms = k_uptime_get();

        if (!in_peak && (after_bpf > t_peak)) {
            in_peak = true;

            bool refractory_ok = (last_peak_ms == 0) ||
                                  ((now_ms - last_peak_ms) >= (int64_t)MIN_RR_MS);

            if (refractory_ok) {
                /* ── Confirmed peak ── */
                float32_t rr_ms = (last_peak_ms == 0)
                                  ? 0.0f
                                  : (float32_t)(now_ms - last_peak_ms);
                last_peak_ms = now_ms;

                if (rr_ms > 0.0f) {
                    live_bpm = 60000.0f / rr_ms;
                    hrv_update(rr_ms);
                    /* VR estimate: BPM / ECG-to-ventilation ratio */
                    live_vr = live_bpm / prof->ecg_vr_ratio;
                }

                /* Cough detection */
                if (after_bpf > t_cough) {
                    cough_record(now_ms);
                    LOG_INF("COUGH: amplitude=%.2f mV, T_cough=%.2f mV",
                            (double)after_bpf, (double)t_cough);
                }

                /* Health classification (re-lock for active_profile read) */
                k_mutex_lock(&profile_mutex, K_FOREVER);
                health_state_t hs = classify_health(now_ms);
                uint8_t sid       = active_profile->id;
                k_mutex_unlock(&profile_mutex);

                if (hs != STATUS_NORMAL) {
                    LOG_WRN("HEALTH ALERT: state=%d BPM=%.1f VR=%.1f "
                            "HRV=%.2f Cough=%.1f/min",
                            (int)hs, (double)live_bpm, (double)live_vr,
                            (double)live_hrv, (double)live_cough);
                }

                /* Toggle LED on each detected peak → visual heart rate blink */
                if (atomic_get(&acq_running)) {
                    gpio_pin_toggle_dt(&led_status);
                }

                LOG_DBG("PEAK: RR=%.1f ms BPM=%.1f HRV=%.2f VR=%.1f",
                        (double)rr_ms, (double)live_bpm,
                        (double)live_hrv,  (double)live_vr);

                /* Pack and enqueue telemetry */
                telemetry_t telem = {
                    .bpm         = live_bpm,
                    .hrv_rmssd   = live_hrv,
                    .vr          = live_vr,
                    .cough_rate  = live_cough,
                    .filtered_mv = after_bpf,
                    .health_state= (uint8_t)hs,
                    .species_id  = sid,
                    .reserved    = {0}
                };

                int ret = k_msgq_put(&telem_queue, &telem, K_NO_WAIT);
                if (ret != 0) {
                    LOG_WRN("telem_queue full — packet dropped");
                }
            }

        } else if (in_peak && (after_bpf <= t_peak)) {
            in_peak = false;   /* falling edge — ready for next peak */
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * BLE THREAD
 *
 * Dequeues telemetry_t structs and sends them as GATT notifications.
 * The telemetry characteristic value attribute is at index 4.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void ble_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    LOG_INF("BLE thread ready");

    /*
     * attrs[] index map (v3):
     *   [0] Primary Service
     *   [1] Species char decl   [2] Species char value
     *   [3] Telem char decl     [4] Telem char value   ← bt_gatt_notify target
     *   [5] Telem CCC
     *   [6] Acq ctrl char decl  [7] Acq ctrl char value
     */
    const struct bt_gatt_attr *telem_attr = &aquabio_svc.attrs[4];

    telemetry_t telem;

    while (true) {
        k_msgq_get(&telem_queue, &telem, K_FOREVER);

        if ((current_conn == NULL) || (!notify_enabled)) {
            continue;   /* drain silently when no central is subscribed */
        }

        int ret = bt_gatt_notify(current_conn, telem_attr,
                                 &telem, sizeof(telem));
        if (ret == -ENOMEM) {
            LOG_WRN("BLE TX buffer full — packet dropped");
        } else if (ret < 0 && ret != -ENOTCONN) {
            LOG_ERR("bt_gatt_notify error (err %d)", ret);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HELPER: GPIO INITIALISATION
 *
 * v3: configures only two GPIOs:
 *   1. Status LED (P0.06)   — output LOW (off at boot)
 *   2. AD8232 SDN (P0.13)  — output HIGH (amplifier enabled at boot)
 *
 * No physical button — acquisition is BLE-commanded.
 * ═══════════════════════════════════════════════════════════════════════════ */
static int gpio_init(void)
{
    int ret;

    /* ── Status LED (P0.06, ACTIVE_HIGH) ── */
    if (!gpio_is_ready_dt(&led_status)) {
        LOG_ERR("Status LED GPIO not ready (P0.06)");
        return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&led_status, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("LED configure failed (err %d)", ret);
        return ret;
    }

    /* ── AD8232 SDN (P0.13, ACTIVE_HIGH) ──────────────────────────────────
     *
     * GPIO_OUTPUT_ACTIVE sets the pin to its active level (HIGH for
     * GPIO_ACTIVE_HIGH) immediately — the AD8232 wakes up before main()
     * proceeds.  The datasheet specifies < 1 ms startup time.
     * -------------------------------------------------------------------- */
    if (!gpio_is_ready_dt(&ad8232_sdn)) {
        LOG_ERR("AD8232 SDN GPIO not ready (P0.13)");
        return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&ad8232_sdn, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        LOG_ERR("AD8232 SDN configure failed (err %d)", ret);
        return ret;
    }

    LOG_INF("GPIO: status LED (P0.06) OFF, AD8232 SDN (P0.13) HIGH — amp enabled");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HELPER: BLE INITIALISATION
 * ═══════════════════════════════════════════════════════════════════════════ */
static int ble_init(void)
{
    int ret = bt_enable(NULL);
    if (ret < 0) { LOG_ERR("bt_enable failed (err %d)", ret); return ret; }
    LOG_INF("Bluetooth stack ready");

    ret = bt_le_adv_start(
        BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,
                        BT_GAP_ADV_FAST_INT_MIN_2,
                        BT_GAP_ADV_FAST_INT_MAX_2,
                        NULL),
        ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (ret < 0) { LOG_ERR("Advertising start failed (err %d)", ret); return ret; }

    LOG_INF("BLE advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN ENTRY POINT
 *
 * Boot sequence:
 *   1. gpio_init()          — LED off, AD8232 SDN HIGH (amp on)
 *   2. ble_init()           — BT stack + start advertising
 *   3. ADC device check
 *   4. nvs_init_and_load()  — load saved species ID from flash
 *   5. dsp_filters_init()   — init notch + species BPF (uses loaded profile)
 *   6. Spawn 3 threads       — all block until BLE commands arrive
 *   7. return 0             — Zephyr takes over
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    LOG_INF("╔══════════════════════════════════════════╗");
    LOG_INF("║  Aquatic Biomonitor Firmware  v3.0.0     ║");
    LOG_INF("╚══════════════════════════════════════════╝");
    LOG_INF("Build: %s %s", __DATE__, __TIME__);

    const struct gpio_dt_spec sdn_pin = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), sdn_gpios);
    if (gpio_is_ready_dt(&sdn_pin)) {
        gpio_pin_configure_dt(&sdn_pin, GPIO_OUTPUT_ACTIVE);
    }

    /* 1. GPIO — LED off, AD8232 amplifier enabled */
    int ret = gpio_init();
    if (ret < 0) { LOG_ERR("GPIO init failed"); return ret; }

    /* 2. BLE stack + advertising */
    ret = ble_init();
    if (ret < 0) { LOG_ERR("BLE init failed"); return ret; }

    /* 3. ADC device readiness check */
    if (!adc_is_ready_dt(&adc_channel)) {
        LOG_ERR("ADC device not ready: %s", adc_channel.dev->name);
        return -ENODEV;
    }
    LOG_INF("ADC ready: %s (AIN0 / P0.02)", adc_channel.dev->name);

    /* 4. NVS — restore last species, or write default */
    ret = nvs_init_and_load();
    if (ret < 0) {
        LOG_WRN("NVS unavailable — using default species (err %d)", ret);
        /* Non-fatal: firmware continues with in-RAM default profile */
    }

    /* 5. DSP filter init — must follow NVS load so profile is correct */
    dsp_filters_init();

    /* 6. Spawn application threads */
    k_thread_create(&adc_thread_data, adc_stack,
                    K_THREAD_STACK_SIZEOF(adc_stack),
                    adc_thread_fn, NULL, NULL, NULL,
                    ADC_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&adc_thread_data, "adc_thread");

    k_thread_create(&dsp_thread_data, dsp_stack,
                    K_THREAD_STACK_SIZEOF(dsp_stack),
                    dsp_thread_fn, NULL, NULL, NULL,
                    DSP_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&dsp_thread_data, "dsp_thread");

    k_thread_create(&ble_thread_data, ble_stack,
                    K_THREAD_STACK_SIZEOF(ble_stack),
                    ble_thread_fn, NULL, NULL, NULL,
                    BLE_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&ble_thread_data, "ble_thread");

    LOG_INF("Active profile : [ID=%u] %s", active_profile->id, active_profile->name);
    LOG_INF("All threads running — connect BLE and write 0x01 to char ...CDEF3 to start");

    /* 7. Return — Zephyr scheduler takes over */
    return 0;
}
