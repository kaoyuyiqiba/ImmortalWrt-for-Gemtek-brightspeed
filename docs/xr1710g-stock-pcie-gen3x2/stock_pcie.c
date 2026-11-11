/* Behavioral reconstruction from the XR1710G stock and engineering kernel images.
 * Register offsets below are relative to symbolic banks; see README.md.
 */
#include "stock_pcie.h"

#include <errno.h>

#define BIT(n) (UINT32_C(1) << (n))
#define SCU_PCIC 0x088u
#define SCU_SSTR 0x09cu
#define SCU_LANE_MODE 0x003u
#define SCU_PERST_X2 (BIT(29) | BIT(26))
#define SCU_PERST_AUX BIT(16)
#define SCU_RESET_830 0x830u
#define SCU_RESET_834 0x834u
#define MAC_SETTING 0x080u
#define MAC_CLASS 0x09cu
#define MAC_RC_CONTROL 0x180u
#define MAC_RC_CONTROL2 0x1acu
#define MAC_EQ_PRESET 0x100u
#define MAC_LTSSM 0x150u
#define MAC_LINK_STATUS 0x154u
#define MAC_PIPE_EQ 0x338u
#define MAC_RX_DETECT 0x10044u
#define MAC_RX_AEQ_L0 0x15030u
#define MAC_RX_AEQ_L1 0x15130u
#define PMA_FREQ_CYCLES 0x14cu
#define PMA_LOCK_WINDOW 0x150u
#define PMA_UNLOCK_WINDOW 0x154u
#define PMA_FREQ_CONTROL 0x158u
#define PMA_FREQ_STATUS 0x530u
#define PMA_IDAC_FORCE 0x794u
#define PMA_POWER_FORCE 0x824u
#define PMA_LPF_FORCE 0x820u
#define PMA_GEN2_IDAC_EXTRA 0x174u
#define PMA_GEN12_IDAC 0x8bcu
#define PMA_GEN3_IDAC 0x8c0u
#define PMA_RX_SPEED_FORCE 0x8b8u
#define PMA_IDAC_MASK 0x7ffu

static void wr(const struct xr_pcie_io *io, enum xr_pcie_bank bank,
               uint32_t reg, uint32_t value)
{
    io->write32(io->ctx, bank, reg, value);
}

static uint32_t rd(const struct xr_pcie_io *io, enum xr_pcie_bank bank, uint32_t reg)
{
    return io->read32(io->ctx, bank, reg);
}

static void update(const struct xr_pcie_io *io, enum xr_pcie_bank bank,
                   uint32_t reg, uint32_t mask, uint32_t value)
{
    wr(io, bank, reg, (rd(io, bank, reg) & ~mask) | (value & mask));
}

static void delay(const struct xr_pcie_io *io, unsigned us)
{
    io->delay_us(io->ctx, us);
}

static uint32_t measure(const struct xr_pcie_io *io, enum xr_pcie_bank pma)
{
    update(io, pma, PMA_FREQ_CONTROL, 7, 0);
    update(io, pma, PMA_FREQ_CONTROL, 7, 3);
    delay(io, 10000);
    return rd(io, pma, PMA_FREQ_STATUS);
}

/* Engineering 0x33baa0; retail 0x3698d0. Both IDAC arrays are 1..7. */
static struct xr_cal_result calibrate(const struct xr_pcie_io *io,
                                      unsigned lane, unsigned gen)
{
    const enum xr_pcie_bank pma = lane == 0 ? XR_PMA0 : XR_PMA1;
    const uint32_t inject = lane == 0 ? 0x11cu : 0x1d4u;
    const uint32_t target = gen == 3 ? 41600u : 41941u;
    const uint32_t cycles = gen == 3 ? 26000u : 32767u;
    struct xr_cal_result result = {0};

    update(io, pma, 0x004, BIT(0), BIT(0));
    update(io, pma, PMA_LOCK_WINDOW, 0xffff, target - 100);
    update(io, pma, PMA_LOCK_WINDOW, 0xffff0000, (target + 100) << 16);
    update(io, pma, PMA_FREQ_CYCLES, 0xffff, cycles);
    update(io, pma, PMA_FREQ_CONTROL, 0xf00, 0x300);
    update(io, pma, PMA_UNLOCK_WINDOW, 0xffff, target - 100);
    update(io, pma, PMA_UNLOCK_WINDOW, 0xffff0000, (target + 100) << 16);
    update(io, pma, PMA_FREQ_CYCLES, 0xffff0000, cycles << 16);
    update(io, pma, PMA_FREQ_CONTROL, 0xf000, 0x3000);
    update(io, XR_CSR_2L, inject, BIT(24), BIT(24));
    update(io, pma, PMA_LPF_FORCE, BIT(24), BIT(24));
    update(io, pma, PMA_LPF_FORCE, BIT(16), BIT(16));
    update(io, pma, PMA_LPF_FORCE, BIT(8), BIT(8));
    update(io, pma, PMA_LPF_FORCE, BIT(0), 0);
    update(io, pma, PMA_IDAC_FORCE, BIT(16), BIT(16));
    update(io, pma, PMA_POWER_FORCE, BIT(24), BIT(24));
    update(io, pma, PMA_POWER_FORCE, BIT(16), 0);
    update(io, pma, PMA_POWER_FORCE, BIT(16), BIT(16));

    for (uint32_t coarse = 1; coarse <= 7; ++coarse) {
        uint32_t candidate = coarse << 8;
        update(io, pma, PMA_IDAC_FORCE, PMA_IDAC_MASK, candidate);
        if ((measure(io, pma) >> 16) > target)
            result.idac = candidate;
    }

    for (unsigned bit = 8; bit-- > 0;) {
        uint32_t candidate = result.idac | BIT(bit);
        update(io, pma, PMA_IDAC_FORCE, PMA_IDAC_MASK, candidate);
        if ((measure(io, pma) >> 16) >= target)
            result.idac = candidate;
    }

    update(io, pma, PMA_IDAC_FORCE, PMA_IDAC_MASK, result.idac);
    /* 0x33bea0..0x33bf28: both branches decrement the counter. Stock
     * reports each unlocked sample and does not exit early on lock. */
    for (unsigned i = 0; i < 10; ++i) {
        result.fl_out = measure(io, pma) >> 16;
        result.last_locked = (rd(io, pma, PMA_FREQ_STATUS) & BIT(0)) != 0;
        result.unlocked_samples += !result.last_locked;
        ++result.attempts;
    }

    update(io, XR_CSR_2L, inject, BIT(24), 0);
    update(io, pma, PMA_LPF_FORCE, BIT(16), BIT(16));
    update(io, pma, PMA_LPF_FORCE, BIT(24), BIT(24));
    update(io, pma, PMA_LPF_FORCE, BIT(8), 0);
    update(io, pma, PMA_POWER_FORCE, BIT(24), 0);
    update(io, pma, PMA_IDAC_FORCE, BIT(16), 0);

    if (gen == 3) {
        update(io, pma, PMA_GEN3_IDAC, PMA_IDAC_MASK, result.idac);
    } else {
        /* Stock has an extra write at PMA+0x174; public B17 has no
         * corresponding write in its RX pre-calibration function. */
        update(io, pma, PMA_GEN2_IDAC_EXTRA, PMA_IDAC_MASK, result.idac);
        update(io, pma, PMA_GEN12_IDAC, PMA_IDAC_MASK, result.idac);
        update(io, pma, PMA_GEN12_IDAC, PMA_IDAC_MASK << 16, result.idac << 16);
    }
    return result;
}

static void calibrate_both_lanes(const struct xr_pcie_io *io,
                                 struct xr_pcie_result *result)
{
    /* Engineering 0x33e550..0x33e790: dual-lane K-flow tail. */
    for (enum xr_pcie_bank pma = XR_PMA0; pma <= XR_PMA1; ++pma)
        update(io, pma, 0x19c, BIT(0), BIT(0));
    for (enum xr_pcie_bank pma = XR_PMA0; pma <= XR_PMA1; ++pma)
        update(io, pma, PMA_GEN3_IDAC, BIT(16), 0);
    for (enum xr_pcie_bank pma = XR_PMA0; pma <= XR_PMA1; ++pma)
        update(io, pma, PMA_RX_SPEED_FORCE, 0xf0, 0xa0);
    for (unsigned lane = 0; lane < 2; ++lane)
        result->gen3[lane] = calibrate(io, lane, 3);
    for (enum xr_pcie_bank pma = XR_PMA0; pma <= XR_PMA1; ++pma)
        update(io, pma, PMA_RX_SPEED_FORCE, 0xf0, 0);
    delay(io, 100);
    for (unsigned lane = 0; lane < 2; ++lane)
        result->gen2[lane] = calibrate(io, lane, 2);
    for (enum xr_pcie_bank pma = XR_PMA0; pma <= XR_PMA1; ++pma)
        update(io, pma, PMA_GEN3_IDAC, BIT(20), 0);
    for (enum xr_pcie_bank pma = XR_PMA0; pma <= XR_PMA1; ++pma)
        update(io, pma, PMA_GEN3_IDAC, BIT(20), BIT(20));
    /* 0x33e424..0x33e4c8: pulse CDR PR power on both lanes. */
    for (enum xr_pcie_bank pma = XR_PMA0; pma <= XR_PMA1; ++pma)
        update(io, pma, 0x34c, BIT(8), 0);
    delay(io, 10);
    for (enum xr_pcie_bank pma = XR_PMA0; pma <= XR_PMA1; ++pma)
        update(io, pma, 0x34c, BIT(8), BIT(8));
    delay(io, 100);
}

int xr_stock_pcie_boot_x2(const struct xr_pcie_io *io, const struct xr_pcie_board *board,
                         struct xr_pcie_result *result)
{
    uint32_t reset_834;
    int ret;

    if (!io || !io->read32 || !io->write32 || !io->delay_us ||
        !io->prepare_shared_phy || !board || !result)
        return -EINVAL;
    *result = (struct xr_pcie_result){0};

    /* Do not infer these inputs from the modern DTS. Mode 1 uses a
     * different topology/relink path; HIR 0xe + package 5 masks Gen3. */
    if ((board->image != XR_STOCK_5112 && board->image != XR_ENGINEERING) ||
        board->wifi0_mode != 0 || board->wifi1_mode != 0 ||
        (board->hir == 0xe && board->package_id == 5))
        return -EOPNOTSUPP;

    /* Probe 0x3417c0..0x341830: port 0 then port 1 mode selection. */
    update(io, XR_SCU, SCU_SSTR, BIT(14) | BIT(13), 0);
    update(io, XR_SCU, SCU_PCIC, SCU_LANE_MODE, 2);
    update(io, XR_SCU, SCU_SSTR, BIT(12) | BIT(11), 0);
    reset_834 = rd(io, XR_SCU, SCU_RESET_834);
    if (board->image == XR_ENGINEERING)
        wr(io, XR_SCU, SCU_RESET_834, reset_834 | 0x2c000000);
    wr(io, XR_SCU, SCU_RESET_834, reset_834 | 0x0c000000);
    update(io, XR_SCU, SCU_RESET_830, BIT(27), BIT(27));
    delay(io, 100000);
    update(io, XR_SCU, SCU_PCIC, SCU_PERST_X2, 0);
    update(io, XR_SCU, SCU_PCIC, SCU_PERST_AUX, 0);
    delay(io, 1000);

    wr(io, XR_MAC0, MAC_RX_DETECT, 0x23020133);
    wr(io, XR_MAC1, MAC_RX_DETECT, 0x23020133);
    wr(io, XR_MAC0, MAC_RX_AEQ_L0, 0x50500032);
    wr(io, XR_MAC0, MAC_RX_AEQ_L1, 0x50500032);

    ret = io->prepare_shared_phy(io->ctx);
    if (ret)
        return ret;
    calibrate_both_lanes(io, result);
    delay(io, 30000);

    update(io, XR_SCU, SCU_RESET_834,
           board->image == XR_ENGINEERING ? 0x2c000000u : 0x0c000000u, 0);
    update(io, XR_SCU, SCU_RESET_830, BIT(27), 0);
    wr(io, XR_MAC0, MAC_EQ_PRESET, 0x41474147);
    wr(io, XR_MAC1, MAC_EQ_PRESET, 0x41474147);
    wr(io, XR_MAC0, MAC_PIPE_EQ, 0x1018020f);
    wr(io, XR_MAC1, MAC_PIPE_EQ, 0x1018020f);
    delay(io, 10000);
    update(io, XR_SCU, SCU_PCIC, SCU_PERST_X2, SCU_PERST_X2);
    update(io, XR_SCU, SCU_PCIC, SCU_PERST_AUX, SCU_PERST_AUX);
    /* Engineering dwell; this reference also uses it in the retail model
     * instead of the retail-only diagnostic LTSSM capture loop. */
    delay(io, 1000000);

    /* Probe 0x341328..0x341370: program main MAC as a root port. */
    update(io, XR_MAC0, MAC_SETTING, BIT(0), BIT(0));
    update(io, XR_MAC0, MAC_CLASS, 0xffffff00, 0x06040000);
    update(io, XR_MAC0, MAC_RC_CONTROL, 0x0f000000, 0x0f000000);
    update(io, XR_MAC0, MAC_RC_CONTROL2, BIT(3), BIT(3));
    /* Probe uses a 100ms wall-clock budget and 6..20us sleep range.
     * The reference's fixed 10us ticks model that budget for fake I/O. */
    for (unsigned elapsed = 0; ; elapsed += 10) {
        result->mac_link_status = rd(io, XR_MAC0, MAC_LINK_STATUS);
        if (result->mac_link_status & BIT(8))
            break;
        if (elapsed == 100000)
            break;
        delay(io, 10);
    }
    result->mac_ltssm = rd(io, XR_MAC0, MAC_LTSSM);
    return (result->mac_link_status & BIT(8)) ? 0 : -ETIMEDOUT;
}
