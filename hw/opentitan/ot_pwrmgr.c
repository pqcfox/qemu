/*
 * QEMU OpenTitan Power Manager device
 *
 * Copyright (c) 2023-2024 Rivos, Inc.
 *
 * Author(s):
 *  Emmanuel Blot <eblot@rivosinc.com>
 *  Loïc Lefort <loic@rivosinc.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Note: for now, only a minimalist subset of Power Manager device is
 *       implemented in order to enable OpenTitan's ROM boot to progress
 */

#include "qemu/osdep.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qapi/error.h"
#include "hw/core/cpu.h"
#include "hw/opentitan/ot_alert.h"
#include "hw/opentitan/ot_common.h"
#include "hw/opentitan/ot_pwrmgr.h"
#include "hw/opentitan/ot_rstmgr.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/registerfields.h"
#include "hw/riscv/ibex_common.h"
#include "hw/riscv/ibex_irq.h"
#include "hw/sysbus.h"
#include "trace.h"

#define PARAM_NUM_RST_REQS       2u
#define PARAM_NUM_INT_RST_REQS   2u
#define PARAM_NUM_DEBUG_RST_REQS 1u
#define PARAM_RESET_MAIN_PWR_IDX 2u
#define PARAM_RESET_ESC_IDX      3u
#define PARAM_RESET_NDM_IDX      4u
#define PARAM_NUM_ALERTS         1u

/* clang-format off */
REG32(INTR_STATE, 0x0u)
    SHARED_FIELD(WAKEUP, 0u, 1u)
REG32(INTR_ENABLE, 0x4u)
REG32(INTR_TEST, 0x8u)
REG32(ALERT_TEST, 0xcu)
    FIELD(ALERT_TEST, FATAL_FAULT, 0u, 1u)
REG32(CTRL_CFG_REGWEN, 0x10u)
    FIELD(CTRL_CFG_REGWEN, EN, 0u, 1u)
REG32(CONTROL, 0x14u)
    FIELD(CONTROL, LOW_POWER_HINT, 0u, 1u)
    FIELD(CONTROL, CORE_CLK_EN, 4u, 1u)
    FIELD(CONTROL, IO_CLK_EN, 5u, 1u)
    FIELD(CONTROL, USB_CLK_EN_LP, 6u, 1u)
    FIELD(CONTROL, USB_CLK_EN_ACTIVE, 7u, 1u)
    FIELD(CONTROL, MAIN_PD_N, 8u, 1u)
REG32(CFG_CDC_SYNC, 0x18u)
    FIELD(CFG_CDC_SYNC, SYNC, 0u, 1u)
REG32(WAKEUP_EN_REGWEN, 0x1cu)
    FIELD(WAKEUP_EN_REGWEN, EN, 0u, 1u)
REG32(WAKEUP_EN, 0x20u)
    SHARED_FIELD(WAKEUP_CHANNEL_0, 0u, 1u)
    SHARED_FIELD(WAKEUP_CHANNEL_1, 1u, 1u)
    SHARED_FIELD(WAKEUP_CHANNEL_2, 2u, 1u)
    SHARED_FIELD(WAKEUP_CHANNEL_3, 3u, 1u)
    SHARED_FIELD(WAKEUP_CHANNEL_4, 4u, 1u)
    SHARED_FIELD(WAKEUP_CHANNEL_5, 5u, 1u)
REG32(WAKE_STATUS, 0x24u)
REG32(RESET_EN_REGWEN, 0x28u)
    FIELD(RESET_EN_REGWEN, EN, 0u, 1u)
REG32(RESET_EN, 0x2cu)
    SHARED_FIELD(RESET_CHANNEL_0, 0u, 1u)
    SHARED_FIELD(RESET_CHANNEL_1, 1u, 1u)
REG32(RESET_STATUS, 0x30u)
REG32(ESCALATE_RESET_STATUS, 0x34u)
    FIELD(ESCALATE_RESET_STATUS, VAL, 0u, 1u)
REG32(WAKE_INFO_CAPTURE_DIS, 0x38u)
    FIELD(WAKE_INFO_CAPTURE_DIS, VAL, 0u, 1u)
REG32(WAKE_INFO, 0x3cu)
    FIELD(WAKE_INFO, REASONS, 0u, 5u)
    FIELD(WAKE_INFO, FALL_THROUGH, 6u, 1u)
    FIELD(WAKE_INFO, ABORT, 7u, 1u)
REG32(FAULT_STATUS, 0x40u)
    FIELD(FAULT_STATUS, REG_INTG_ERR, 0u, 1u)
    FIELD(FAULT_STATUS, ESC_TIMEOUT, 1u, 1u)
    FIELD(FAULT_STATUS, MAIN_PD_GLITCH, 2u, 1u)
/* clang-format on */

#define CONTROL_MASK \
    (R_CONTROL_LOW_POWER_HINT_MASK | R_CONTROL_CORE_CLK_EN_MASK | \
     R_CONTROL_IO_CLK_EN_MASK | R_CONTROL_USB_CLK_EN_LP_MASK | \
     R_CONTROL_USB_CLK_EN_ACTIVE_MASK | R_CONTROL_MAIN_PD_N_MASK)
#define WAKEUP_MASK \
    (WAKEUP_CHANNEL_0_MASK | WAKEUP_CHANNEL_1_MASK | WAKEUP_CHANNEL_2_MASK | \
     WAKEUP_CHANNEL_3_MASK | WAKEUP_CHANNEL_4_MASK | WAKEUP_CHANNEL_5_MASK)
#define RESET_MASK (RESET_CHANNEL_0_MASK | RESET_CHANNEL_1_MASK)
#define WAKE_INFO_MASK \
    (R_WAKE_INFO_REASONS_MASK | R_WAKE_INFO_FALL_THROUGH_MASK | \
     R_WAKE_INFO_ABORT_MASK)

#define CDC_SYNC_PULSE_DURATION_NS 100000u /* 100us */

/* Verbatim definitions from RTL */
#define NUM_SW_RST_REQ 1u
#define HW_RESET_WIDTH \
    (PARAM_NUM_RST_REQS + PARAM_NUM_INT_RST_REQS + PARAM_NUM_DEBUG_RST_REQS)
#define TOTAL_RESET_WIDTH (HW_RESET_WIDTH + NUM_SW_RST_REQ)
#define RESET_SW_REQ_IDX  (TOTAL_RESET_WIDTH - 1u)

#define R32_OFF(_r_) ((_r_) / sizeof(uint32_t))

#define R_LAST_REG (R_FAULT_STATUS)
#define REGS_COUNT (R_LAST_REG + 1u)
#define REGS_SIZE  (REGS_COUNT * sizeof(uint32_t))
#define REG_NAME(_reg_) \
    ((((_reg_) <= REGS_COUNT) && REG_NAMES[_reg_]) ? REG_NAMES[_reg_] : "?")

#define REG_NAME_ENTRY(_reg_) [R_##_reg_] = stringify(_reg_)
static const char *REG_NAMES[REGS_COUNT] = {
    REG_NAME_ENTRY(INTR_STATE),
    REG_NAME_ENTRY(INTR_ENABLE),
    REG_NAME_ENTRY(INTR_TEST),
    REG_NAME_ENTRY(ALERT_TEST),
    REG_NAME_ENTRY(CTRL_CFG_REGWEN),
    REG_NAME_ENTRY(CONTROL),
    REG_NAME_ENTRY(CFG_CDC_SYNC),
    REG_NAME_ENTRY(WAKEUP_EN_REGWEN),
    REG_NAME_ENTRY(WAKEUP_EN),
    REG_NAME_ENTRY(WAKE_STATUS),
    REG_NAME_ENTRY(RESET_EN_REGWEN),
    REG_NAME_ENTRY(RESET_EN),
    REG_NAME_ENTRY(RESET_STATUS),
    REG_NAME_ENTRY(ESCALATE_RESET_STATUS),
    REG_NAME_ENTRY(WAKE_INFO_CAPTURE_DIS),
    REG_NAME_ENTRY(WAKE_INFO),
    REG_NAME_ENTRY(FAULT_STATUS),
};
#undef REG_NAME_ENTRY

/* not a real register, but a way to store incoming signals */
SHARED_FIELD(INPUTS_LC, 0u, 1u)
SHARED_FIELD(INPUTS_OTP, 1u, 1u)

typedef enum {
    OT_PWRMGR_INIT_OTP,
    OT_PWRMGR_INIT_LC_CTRL,
    OT_PWRMGR_INIT_COUNT,
} OtPwrMgrInit;

typedef enum {
    OT_PWR_FAST_ST_LOW_POWER,
    OT_PWR_FAST_ST_ENABLE_CLOCKS,
    OT_PWR_FAST_ST_RELEASE_LC_RST,
    OT_PWR_FAST_ST_OTP_INIT,
    OT_PWR_FAST_ST_LC_INIT,
    OT_PWR_FAST_ST_STRAP,
    OT_PWR_FAST_ST_ACK_PWR_UP,
    OT_PWR_FAST_ST_ROM_CHECK_DONE,
    OT_PWR_FAST_ST_ROM_CHECK_GOOD,
    OT_PWR_FAST_ST_ACTIVE,
    OT_PWR_FAST_ST_DIS_CLKS,
    OT_PWR_FAST_ST_FALL_THROUGH,
    OT_PWR_FAST_ST_NVM_IDLE_CHK,
    OT_PWR_FAST_ST_LOW_POWER_PREP,
    OT_PWR_FAST_ST_NVM_SHUT_DOWN, /* not used in DJ */
    OT_PWR_FAST_ST_RESET_PREP,
    OT_PWR_FAST_ST_RESET_WAIT,
    OT_PWR_FAST_ST_REQ_PWR_DN,
    OT_PWR_FAST_ST_INVALID,
} OtPwrMgrFastState;

typedef enum {
    OT_PWR_SLOW_ST_RESET,
    OT_PWR_SLOW_ST_LOW_POWER,
    OT_PWR_SLOW_ST_MAIN_POWER_ON,
    OT_PWR_SLOW_ST_PWR_CLAMP_OFF,
    OT_PWR_SLOW_ST_CLOCKS_ON,
    OT_PWR_SLOW_ST_REQ_PWR_UP,
    OT_PWR_SLOW_ST_IDLE,
    OT_PWR_SLOW_ST_ACK_PWR_DN,
    OT_PWR_SLOW_ST_CLOCKS_OFF,
    OT_PWR_SLOW_ST_PWR_CLAMP_ON,
    OT_PWR_SLOW_ST_MAIN_POWER_OFF,
    OT_PWR_SLOW_ST_INVALID,
} OtPwrMgrSlowState;

typedef struct {
    bool good;
    bool done;
} OtPwrMgrRomStatus;

typedef enum {
    OT_PWRMGR_SLOW_DOMAIN,
    OT_PWRMGR_FAST_DOMAIN,
} OtPwrMgrClockDomain;

typedef struct {
    OtRstMgrResetReq req;
    OtPwrMgrClockDomain domain;
} OtPwrMgrResetReq;

struct OtPwrMgrState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    QEMUTimer *cdc_sync;
    QEMUBH *fsm_tick_bh;
    IbexIRQ irq; /* wake from low power */
    IbexIRQ alert;
    IbexIRQ cpu_enable;
    IbexIRQ pwr_lc_req;
    IbexIRQ pwr_otp_req;

    OtPwrMgrFastState f_state;
    OtPwrMgrSlowState s_state;
    unsigned fsm_event_count;
    uint32_t inputs;

    uint32_t *regs;
    OtPwrMgrResetReq reset_req;
    OtPwrMgrRomStatus *roms;

    char *ot_id;
    OtRstMgrState *rstmgr;
    uint8_t num_rom;
};

#define PWRMGR_NAME_ENTRY(_pre_, _name_) [_pre_##_##_name_] = stringify(_name_)

#define FAST_ST_NAME_ENTRY(_name_) PWRMGR_NAME_ENTRY(OT_PWR_FAST_ST, _name_)
/* clang-format off */
static const char *FAST_ST_NAMES[] = {
    FAST_ST_NAME_ENTRY(LOW_POWER),
    FAST_ST_NAME_ENTRY(ENABLE_CLOCKS),
    FAST_ST_NAME_ENTRY(RELEASE_LC_RST),
    FAST_ST_NAME_ENTRY(OTP_INIT),
    FAST_ST_NAME_ENTRY(LC_INIT),
    FAST_ST_NAME_ENTRY(STRAP),
    FAST_ST_NAME_ENTRY(ACK_PWR_UP),
    FAST_ST_NAME_ENTRY(ROM_CHECK_DONE),
    FAST_ST_NAME_ENTRY(ROM_CHECK_GOOD),
    FAST_ST_NAME_ENTRY(ACTIVE),
    FAST_ST_NAME_ENTRY(DIS_CLKS),
    FAST_ST_NAME_ENTRY(FALL_THROUGH),
    FAST_ST_NAME_ENTRY(NVM_IDLE_CHK),
    FAST_ST_NAME_ENTRY(LOW_POWER_PREP),
    FAST_ST_NAME_ENTRY(NVM_SHUT_DOWN),
    FAST_ST_NAME_ENTRY(RESET_PREP),
    FAST_ST_NAME_ENTRY(RESET_WAIT),
    FAST_ST_NAME_ENTRY(REQ_PWR_DN),
    FAST_ST_NAME_ENTRY(INVALID),
};
/* clang-format on */
#undef FAST_ST_NAME_ENTRY
#define FST_NAME(_st_) \
    ((_st_) < ARRAY_SIZE(FAST_ST_NAMES) ? FAST_ST_NAMES[(_st_)] : "?")

#define SLOW_ST_NAME_ENTRY(_name_) PWRMGR_NAME_ENTRY(OT_PWR_SLOW_ST, _name_)
/* clang-format off */
static const char *SLOW_ST_NAMES[] = {
    SLOW_ST_NAME_ENTRY(RESET),
    SLOW_ST_NAME_ENTRY(LOW_POWER),
    SLOW_ST_NAME_ENTRY(MAIN_POWER_ON),
    SLOW_ST_NAME_ENTRY(PWR_CLAMP_OFF),
    SLOW_ST_NAME_ENTRY(CLOCKS_ON),
    SLOW_ST_NAME_ENTRY(REQ_PWR_UP),
    SLOW_ST_NAME_ENTRY(IDLE),
    SLOW_ST_NAME_ENTRY(ACK_PWR_DN),
    SLOW_ST_NAME_ENTRY(CLOCKS_OFF),
    SLOW_ST_NAME_ENTRY(PWR_CLAMP_ON),
    SLOW_ST_NAME_ENTRY(MAIN_POWER_OFF),
    SLOW_ST_NAME_ENTRY(INVALID),
};
/* clang-format on */
#undef SLOW_ST_NAME_ENTRY
#define SST_NAME(_st_) \
    ((_st_) < ARRAY_SIZE(SLOW_ST_NAMES) ? SLOW_ST_NAMES[(_st_)] : "?")

#define WAKEUP_NAME_ENTRY(_name_) PWRMGR_NAME_ENTRY(OT_PWRMGR_WAKEUP, _name_)
/* clang-format off */
static const char *WAKEUP_NAMES[] = {
    WAKEUP_NAME_ENTRY(SYSRST),
    WAKEUP_NAME_ENTRY(ADC_CTRL),
    WAKEUP_NAME_ENTRY(PINMUX),
    WAKEUP_NAME_ENTRY(USBDEV),
    WAKEUP_NAME_ENTRY(AON_TIMER),
    WAKEUP_NAME_ENTRY(SENSOR),
};
/* clang-format on */
#undef WAKEUP_NAME_ENTRY
#define WAKEUP_NAME(_clk_) \
    ((_clk_) < ARRAY_SIZE(WAKEUP_NAMES) ? WAKEUP_NAMES[(_clk_)] : "?")

#define RESET_NAME_ENTRY(_name_) PWRMGR_NAME_ENTRY(OT_PWRMGR_RST, _name_)
/* clang-format off */
static const char *RST_NAMES[] = {
    RESET_NAME_ENTRY(SYSRST),
    RESET_NAME_ENTRY(AON_TIMER),
};
/* clang-format on */
#undef RESET_NAME_ENTRY
#define RST_NAME(_clk_) \
    ((_clk_) < ARRAY_SIZE(RST_NAMES) ? RST_NAMES[(_clk_)] : "?")

#undef PWRMGR_NAME_ENTRY

#define PWR_CHANGE_FAST_STATE(_s_, _st_) \
    ot_pwrmgr_change_fast_state_line(_s_, OT_PWR_FAST_ST_##_st_, __LINE__)

#define PWR_CHANGE_SLOW_STATE(_s_, _st_) \
    ot_pwrmgr_change_slow_state_line(_s_, OT_PWR_SLOW_ST_##_st_, __LINE__)

static void ot_pwrmgr_change_fast_state_line(OtPwrMgrState *s,
                                             OtPwrMgrFastState state, int line)
{
    trace_ot_pwrmgr_change_state(s->ot_id, line, "fast", FST_NAME(s->f_state),
                                 s->f_state, FST_NAME(state), state);

    s->f_state = state;
}

static void ot_pwrmgr_change_slow_state_line(OtPwrMgrState *s,
                                             OtPwrMgrSlowState state, int line)
{
    trace_ot_pwrmgr_change_state(s->ot_id, line, "slow", SST_NAME(s->s_state),
                                 s->s_state, SST_NAME(state), state);

    s->s_state = state;
}

static void ot_pwrmgr_update_irq(OtPwrMgrState *s)
{
    uint32_t level = s->regs[R_INTR_STATE] & s->regs[R_INTR_ENABLE];

    ibex_irq_set(&s->irq, (int)(bool)level);
}

static void ot_pwrmgr_fsm_push_event(OtPwrMgrState *s, bool trigger)
{
    s->fsm_event_count += 1u;
    if (trigger) {
        qemu_bh_schedule(s->fsm_tick_bh);
    }
}

static void ot_pwrmgr_fsm_pop_event(OtPwrMgrState *s)
{
    g_assert(s->fsm_event_count);

    s->fsm_event_count -= 1u;
}

static void ot_pwrmgr_fsm_schedule(OtPwrMgrState *s)
{
    if (s->fsm_event_count) {
        qemu_bh_schedule(s->fsm_tick_bh);
    }
}

static void ot_pwrmgr_cdc_sync(void *opaque)
{
    OtPwrMgrState *s = opaque;

    s->regs[R_CFG_CDC_SYNC] &= ~R_CFG_CDC_SYNC_SYNC_MASK;
}

static void ot_pwrmgr_rom_good(void *opaque, int n, int level)
{
    OtPwrMgrState *s = opaque;

    g_assert((unsigned)n < s->num_rom);

    s->roms[n].good = level;

    trace_ot_pwrmgr_rom(s->ot_id, n, "good", s->roms[n].good);

    ot_pwrmgr_fsm_push_event(s, true);
}

static void ot_pwrmgr_rom_done(void *opaque, int n, int level)
{
    OtPwrMgrState *s = opaque;

    g_assert((unsigned)n < s->num_rom);

    s->roms[n].done = level;

    trace_ot_pwrmgr_rom(s->ot_id, n, "done", s->roms[n].done);

    ot_pwrmgr_fsm_push_event(s, true);
}

static void ot_pwrmgr_wkup(void *opaque, int irq, int level)
{
    OtPwrMgrState *s = opaque;
    unsigned src = (unsigned)irq;

    assert(src < OT_PWRMGR_WAKEUP_COUNT);

    qemu_log_mask(LOG_UNIMP, "%s", __func__);

    trace_ot_pwrmgr_wkup(s->ot_id, WAKEUP_NAME(src), src, (bool)level);
}

static void ot_pwrmgr_rst_req(void *opaque, int irq, int level)
{
    OtPwrMgrState *s = opaque;

    unsigned src = (unsigned)irq;
    g_assert(src < OT_PWRMGR_RST_COUNT);

    uint32_t rstmask = 1u << src; /* rst_req are stored in the LSBs */

    if (level) {
        trace_ot_pwrmgr_rst_req(s->ot_id, RST_NAME(src), src);

        if (s->regs[R_RESET_STATUS]) {
            /* do nothing if a reset is already in progress */
            /* TODO: is it true for HW vs. SW request ?*/
            return;
        }
        s->regs[R_RESET_STATUS] |= rstmask;

        switch (irq) {
        case OT_PWRMGR_RST_SYSRST:
            s->reset_req.req = OT_RSTMGR_RESET_SYSCTRL;
            s->reset_req.domain = OT_PWRMGR_SLOW_DOMAIN;
            break;
        case OT_PWRMGR_RST_AON_TIMER:
            s->reset_req.req = OT_RSTMGR_RESET_AON_TIMER;
            s->reset_req.domain = OT_PWRMGR_SLOW_DOMAIN;
            break;
        default:
            g_assert_not_reached();
            break;
        }

        trace_ot_pwrmgr_reset_req(s->ot_id, "scheduling reset", src);

        ot_pwrmgr_fsm_push_event(s, true);
    }
}

static void ot_pwrmgr_sw_rst_req(void *opaque, int irq, int level)
{
    OtPwrMgrState *s = opaque;

    unsigned src = (unsigned)irq;
    g_assert(src < NUM_SW_RST_REQ);

    uint32_t rstbit = 1u << (NUM_SW_RST_REQ + src);

    if (level) {
        trace_ot_pwrmgr_rst_req(s->ot_id, "SW", src);

        if (!(s->regs[R_RESET_EN] & rstbit)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: SW reset #%u not enabled 0x%08x 0x%08x\n",
                          __func__, src, s->regs[R_RESET_EN], rstbit);
            return;
        }

        if (s->regs[R_RESET_STATUS]) {
            /* do nothing if a reset is already in progress */
            return;
        }
        s->regs[R_RESET_STATUS] |= rstbit;

        s->reset_req.req = OT_RSTMGR_RESET_SW;
        s->reset_req.domain = OT_PWRMGR_FAST_DOMAIN;

        trace_ot_pwrmgr_reset_req(s->ot_id, "scheduling SW reset", 0);
        ot_pwrmgr_fsm_push_event(s, true);
    }
}


static void ot_pwrmgr_slow_fsm_tick(OtPwrMgrState *s)
{
    /* fast forward to IDLE slow FSM state for now */
    if (s->s_state == OT_PWR_SLOW_ST_RESET) {
        PWR_CHANGE_SLOW_STATE(s, IDLE);
    }
}

static void ot_pwrmgr_fast_fsm_tick(OtPwrMgrState *s)
{
    if (s->s_state != OT_PWR_SLOW_ST_IDLE) {
        // TODO: to be handled
        return;
    }

    switch (s->f_state) {
    case OT_PWR_FAST_ST_LOW_POWER:
        PWR_CHANGE_FAST_STATE(s, ENABLE_CLOCKS);
        ot_pwrmgr_fsm_push_event(s, false);
        break;
    case OT_PWR_FAST_ST_ENABLE_CLOCKS:
        PWR_CHANGE_FAST_STATE(s, RELEASE_LC_RST);
        // TODO: need to release ROM controllers from reset here to emulate
        // they are clocked and start to verify their contents.
        ot_pwrmgr_fsm_push_event(s, false);
        break;
    case OT_PWR_FAST_ST_RELEASE_LC_RST:
        PWR_CHANGE_FAST_STATE(s, OTP_INIT);
        ibex_irq_set(&s->pwr_otp_req, (int)true);
        break;
    case OT_PWR_FAST_ST_OTP_INIT:
        if (s->inputs & INPUTS_OTP_MASK) {
            /* release the request signal */
            ibex_irq_set(&s->pwr_otp_req, (int)false);
            PWR_CHANGE_FAST_STATE(s, LC_INIT);
            ibex_irq_set(&s->pwr_lc_req, (int)true);
        }
        break;
    case OT_PWR_FAST_ST_LC_INIT:
        if (s->inputs & INPUTS_LC_MASK) {
            /* release the request signal */
            ibex_irq_set(&s->pwr_lc_req, (int)false);
            PWR_CHANGE_FAST_STATE(s, STRAP);
        }
        break;
    case OT_PWR_FAST_ST_STRAP:
        // TODO: need to sample straps
        PWR_CHANGE_FAST_STATE(s, ACK_PWR_UP);
        ot_pwrmgr_fsm_push_event(s, false);
        break;
    case OT_PWR_FAST_ST_ACK_PWR_UP:
        PWR_CHANGE_FAST_STATE(s, ROM_CHECK_DONE);
        ot_pwrmgr_fsm_push_event(s, false);
        break;
    case OT_PWR_FAST_ST_ROM_CHECK_DONE:
        for (unsigned ix = 0; ix < s->num_rom; ix++) {
            if (!s->roms[ix].done) {
                break;
            }
        }
        PWR_CHANGE_FAST_STATE(s, ROM_CHECK_GOOD);
        break;
    case OT_PWR_FAST_ST_ROM_CHECK_GOOD: {
        bool success = true;
        for (unsigned ix = 0; ix < s->num_rom; ix++) {
            if (!s->roms[ix].good) {
                success = false;
                break;
            }
        }
        if (success) {
            PWR_CHANGE_FAST_STATE(s, ACTIVE);
            ot_pwrmgr_fsm_push_event(s, false);
        }
    } break;
    case OT_PWR_FAST_ST_ACTIVE:
        if (!s->regs[R_RESET_STATUS]) {
            ibex_irq_set(&s->cpu_enable, (int)true);
        } else {
            ibex_irq_set(&s->cpu_enable, (int)false);
            PWR_CHANGE_FAST_STATE(s, DIS_CLKS);
            ot_pwrmgr_fsm_push_event(s, false);
        }
        break;
    case OT_PWR_FAST_ST_DIS_CLKS:
        PWR_CHANGE_FAST_STATE(s, RESET_PREP);
        ot_pwrmgr_fsm_push_event(s, false);
        break;
    case OT_PWR_FAST_ST_FALL_THROUGH:
    case OT_PWR_FAST_ST_NVM_IDLE_CHK:
    case OT_PWR_FAST_ST_LOW_POWER_PREP:
    case OT_PWR_FAST_ST_NVM_SHUT_DOWN:
        qemu_log_mask(LOG_UNIMP, "%s: low power modes are not implemented\n",
                      __func__);
        break;
    case OT_PWR_FAST_ST_RESET_PREP: /* fallthrough */
        PWR_CHANGE_FAST_STATE(s, RESET_WAIT);
        ot_rstmgr_reset_req(s->rstmgr, (bool)s->reset_req.domain,
                            s->reset_req.req);
        break;
    /* NOLINTNEXTLINE */
    case OT_PWR_FAST_ST_RESET_WAIT:
        /* wait here for QEMU to reset the Power Manager */
        break;
    case OT_PWR_FAST_ST_REQ_PWR_DN:
    case OT_PWR_FAST_ST_INVALID:
        break;
    }
}

static void ot_pwrmgr_fsm_tick(void *opaque)
{
    OtPwrMgrState *s = opaque;

    ot_pwrmgr_fsm_pop_event(s);

    ot_pwrmgr_slow_fsm_tick(s);
    ot_pwrmgr_fast_fsm_tick(s);

    if (s->f_state != OT_PWR_FAST_ST_INVALID &&
        s->s_state != OT_PWR_SLOW_ST_INVALID) {
        ot_pwrmgr_fsm_schedule(s);
    }
}

/*
 * Input lines
 */

static void ot_pwrmgr_pwr_lc_rsp(void *opaque, int n, int level)
{
    OtPwrMgrState *s = opaque;

    g_assert(n == 0);

    if (level == 1) {
        s->inputs |= INPUTS_LC_MASK;
        ot_pwrmgr_fsm_push_event(s, true);
    }
}

static void ot_pwrmgr_pwr_otp_rsp(void *opaque, int n, int level)
{
    OtPwrMgrState *s = opaque;

    g_assert(n == 0);

    if (level == 1) {
        s->inputs |= INPUTS_OTP_MASK;
        ot_pwrmgr_fsm_push_event(s, true);
    }
}

static uint64_t ot_pwrmgr_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    OtPwrMgrState *s = opaque;
    (void)size;
    uint32_t val32;

    hwaddr reg = R32_OFF(addr);

    switch (reg) {
    case R_INTR_STATE:
    case R_INTR_ENABLE:
    case R_CTRL_CFG_REGWEN:
    case R_CONTROL:
    case R_CFG_CDC_SYNC:
    case R_WAKEUP_EN_REGWEN:
    case R_WAKEUP_EN:
    case R_WAKE_STATUS:
    case R_RESET_EN_REGWEN:
    case R_RESET_EN:
    case R_RESET_STATUS:
    case R_ESCALATE_RESET_STATUS:
    case R_WAKE_INFO_CAPTURE_DIS:
    case R_WAKE_INFO:
    case R_FAULT_STATUS:
        val32 = s->regs[reg];
        break;
    case R_INTR_TEST:
    case R_ALERT_TEST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: W/O register 0x%02" HWADDR_PRIx " (%s)\n", __func__,
                      addr, REG_NAME(reg));
        val32 = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        val32 = 0;
        break;
    }

    uint32_t pc = ibex_get_current_pc();
    trace_ot_pwrmgr_io_read_out(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                                pc);

    return (uint64_t)val32;
};

static void ot_pwrmgr_regs_write(void *opaque, hwaddr addr, uint64_t val64,
                                 unsigned size)
{
    OtPwrMgrState *s = opaque;
    (void)size;
    uint32_t val32 = (uint32_t)val64;

    hwaddr reg = R32_OFF(addr);

    uint32_t pc = ibex_get_current_pc();
    trace_ot_pwrmgr_io_write(s->ot_id, (uint32_t)addr, REG_NAME(reg), val32,
                             pc);
    switch (reg) {
    case R_INTR_STATE:
        val32 &= WAKEUP_MASK;
        s->regs[R_INTR_STATE] &= ~val32; /* RW1C */
        ot_pwrmgr_update_irq(s);
        break;
    case R_INTR_ENABLE:
        val32 &= WAKEUP_MASK;
        s->regs[R_INTR_ENABLE] = val32;
        ot_pwrmgr_update_irq(s);
        break;
    case R_INTR_TEST:
        val32 &= WAKEUP_MASK;
        s->regs[R_INTR_STATE] |= val32;
        ot_pwrmgr_update_irq(s);
        break;
    case R_ALERT_TEST:
        val32 &= R_ALERT_TEST_FATAL_FAULT_MASK;
        if (val32) {
            ibex_irq_set(&s->alert, (int)val32);
        }
        break;
    case R_CONTROL:
        /* TODO: clear LOW_POWER_HINT on next WFI? */
        val32 &= CONTROL_MASK;
        s->regs[reg] = val32;
        break;
    case R_CFG_CDC_SYNC:
        val32 &= R_CFG_CDC_SYNC_SYNC_MASK;
        s->regs[reg] |= val32; /* not described as RW1S, but looks like it */
        if (val32) {
            timer_mod(s->cdc_sync, qemu_clock_get_ns(OT_VIRTUAL_CLOCK) +
                                       CDC_SYNC_PULSE_DURATION_NS);
        }
        break;
    case R_WAKEUP_EN_REGWEN:
        val32 &= R_WAKEUP_EN_REGWEN_EN_MASK;
        s->regs[reg] = val32;
        break;
    case R_WAKEUP_EN:
        if (s->regs[R_WAKEUP_EN_REGWEN] & R_WAKEUP_EN_REGWEN_EN_MASK) {
            val32 &= WAKEUP_MASK;
            s->regs[reg] = val32;
        }
        break;
    case R_RESET_EN_REGWEN:
        val32 &= R_RESET_EN_REGWEN_EN_MASK;
        s->regs[reg] = val32;
        break;
    case R_RESET_EN:
        if (s->regs[R_RESET_EN_REGWEN] & R_RESET_EN_REGWEN_EN_MASK) {
            val32 &= RESET_MASK;
            s->regs[reg] = val32;
        }
        break;
    case R_WAKE_INFO_CAPTURE_DIS:
        val32 &= R_WAKE_INFO_CAPTURE_DIS_VAL_MASK;
        s->regs[reg] = val32;
        break;
    case R_WAKE_INFO:
        val32 &= WAKE_INFO_MASK;
        s->regs[reg] &= ~val32; /* RW1C */
        break;
    case R_CTRL_CFG_REGWEN:
    case R_WAKE_STATUS:
    case R_RESET_STATUS:
    case R_ESCALATE_RESET_STATUS:
    case R_FAULT_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: R/O register 0x%02" HWADDR_PRIx " (%s)\n", __func__,
                      addr, REG_NAME(reg));
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
};

static Property ot_pwrmgr_properties[] = {
    DEFINE_PROP_STRING("ot_id", OtPwrMgrState, ot_id),
    DEFINE_PROP_UINT8("num-rom", OtPwrMgrState, num_rom, 0),
    DEFINE_PROP_LINK("rstmgr", OtPwrMgrState, rstmgr, TYPE_OT_RSTMGR,
                     OtRstMgrState *),
    DEFINE_PROP_END_OF_LIST(),
};

static const MemoryRegionOps ot_pwrmgr_regs_ops = {
    .read = &ot_pwrmgr_regs_read,
    .write = &ot_pwrmgr_regs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4u,
    .impl.max_access_size = 4u,
};

static void ot_pwrmgr_reset(DeviceState *dev)
{
    OtPwrMgrState *s = OT_PWRMGR(dev);

    g_assert(s->ot_id);

    trace_ot_pwrmgr_reset(s->ot_id);

    assert(s->rstmgr);

    timer_del(s->cdc_sync);
    memset(s->regs, 0, REGS_SIZE);

    s->regs[R_CTRL_CFG_REGWEN] = 0x1u;
    s->regs[R_CONTROL] = 0x180u;
    s->regs[R_WAKEUP_EN_REGWEN] = 0x1u;
    s->regs[R_RESET_EN_REGWEN] = 0x1u;

    s->inputs = 0;
    s->fsm_event_count = 0;

    PWR_CHANGE_FAST_STATE(s, LOW_POWER);
    PWR_CHANGE_SLOW_STATE(s, RESET);

    ot_pwrmgr_update_irq(s);
    ibex_irq_set(&s->cpu_enable, 0);
    ibex_irq_set(&s->pwr_otp_req, 0);
    ibex_irq_set(&s->pwr_lc_req, 0);
    ibex_irq_set(&s->alert, 0);

    memset(s->roms, 0, s->num_rom * sizeof(OtPwrMgrRomStatus));

    ot_pwrmgr_fsm_push_event(s, true);
}

static void ot_pwrmgr_realize(DeviceState *dev, Error **errp)
{
    OtPwrMgrState *s = OT_PWRMGR(dev);
    (void)errp;

    if (s->num_rom) {
        s->roms = g_new0(OtPwrMgrRomStatus, s->num_rom);

        qdev_init_gpio_in_named(dev, &ot_pwrmgr_rom_good, OT_PWRMGR_ROM_GOOD,
                                s->num_rom);
        qdev_init_gpio_in_named(dev, &ot_pwrmgr_rom_done, OT_PWRMGR_ROM_DONE,
                                s->num_rom);
    } else {
        s->roms = NULL;
    }
}

static void ot_pwrmgr_init(Object *obj)
{
    OtPwrMgrState *s = OT_PWRMGR(obj);

    memory_region_init_io(&s->mmio, obj, &ot_pwrmgr_regs_ops, s, TYPE_OT_PWRMGR,
                          REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    s->regs = g_new0(uint32_t, REGS_COUNT);
    ibex_sysbus_init_irq(obj, &s->irq);
    ibex_qdev_init_irq(obj, &s->alert, OT_DEVICE_ALERT);
    ibex_qdev_init_irq(obj, &s->pwr_lc_req, OT_PWRMGR_LC_REQ);
    ibex_qdev_init_irq(obj, &s->pwr_otp_req, OT_PWRMGR_OTP_REQ);
    ibex_qdev_init_irq(obj, &s->cpu_enable, OT_PWRMGR_CPU_EN);

    s->cdc_sync = timer_new_ns(OT_VIRTUAL_CLOCK, &ot_pwrmgr_cdc_sync, s);

    qdev_init_gpio_in_named(DEVICE(obj), &ot_pwrmgr_wkup, OT_PWRMGR_WKUP,
                            OT_PWRMGR_WAKEUP_COUNT);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_pwrmgr_rst_req, OT_PWRMGR_RST,
                            OT_PWRMGR_RST_COUNT);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_pwrmgr_sw_rst_req,
                            OT_PWRMGR_SW_RST, NUM_SW_RST_REQ);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_pwrmgr_pwr_lc_rsp,
                            OT_PWRMGR_LC_RSP, 1);
    qdev_init_gpio_in_named(DEVICE(obj), &ot_pwrmgr_pwr_otp_rsp,
                            OT_PWRMGR_OTP_RSP, 1);

    s->fsm_tick_bh = qemu_bh_new(&ot_pwrmgr_fsm_tick, s);
}

static void ot_pwrmgr_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;

    dc->realize = &ot_pwrmgr_realize;
    dc->reset = &ot_pwrmgr_reset;
    device_class_set_props(dc, ot_pwrmgr_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ot_pwrmgr_info = {
    .name = TYPE_OT_PWRMGR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OtPwrMgrState),
    .instance_init = &ot_pwrmgr_init,
    .class_init = &ot_pwrmgr_class_init,
};

static void ot_pwrmgr_register_types(void)
{
    type_register_static(&ot_pwrmgr_info);
}

type_init(ot_pwrmgr_register_types);
