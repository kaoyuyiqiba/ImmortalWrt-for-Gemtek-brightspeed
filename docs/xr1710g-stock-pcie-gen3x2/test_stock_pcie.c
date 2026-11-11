#include "stock_pcie.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#define BIT(n) (UINT32_C(1) << (n))
#define WORDS (0x16000u / 4u)
#define PERST_MAIN (BIT(29) | BIT(26))

struct fake {
    uint32_t reg[6][WORDS];
    unsigned elapsed, released_at, reads, cal_starts, writes, reset_writes;
    unsigned samples[2], start_order[4], coarse[2][2];
    bool prepare_seen, fail_prepare, link_never_up, lock_never;
};

static struct fake fake;

static uint32_t read32(void *arg, enum xr_pcie_bank bank, uint32_t offset)
{
    struct fake *f = arg;
    assert((offset & 3u) == 0 && offset / 4u < WORDS);
    if ((bank == XR_PMA0 || bank == XR_PMA1) && offset == 0x530) {
        unsigned lane = bank == XR_PMA1;
        uint32_t start = f->reg[bank][0x150 / 4] & 0xffffu;
        unsigned target = start + 100;
        unsigned idac = f->reg[bank][0x794 / 4] & 0x7ffu;
        unsigned cutoff = target == 41600 ? 0x7afu - lane : 0x56du + lane;
        /* Monotonically decreasing synthetic frequency, with the Gen3
         * threshold in coarse bin 7 so omission of that bin is visible. */
        unsigned frequency = target + cutoff - idac;
        return frequency << 16 | (!f->lock_never && idac == cutoff);
    }
    if (bank == XR_MAC0 && offset == 0x154) {
        assert(f->released_at && f->elapsed >= f->released_at + 1000000);
        assert((f->reg[XR_MAC0][0x9c / 4] >> 8) == 0x060400);
        assert(f->reg[XR_MAC0][0x180 / 4] == 0x0f000000);
        assert(f->reg[XR_MAC0][0x1ac / 4] == BIT(3));
        ++f->reads;
        return !f->link_never_up && f->reads >= 3 ? BIT(8) : 0;
    }
    if (bank == XR_MAC0 && offset == 0x150)
        return f->link_never_up ? 0x01000000u : 0x10000000u;
    return f->reg[bank][offset / 4u];
}

static void write32(void *arg, enum xr_pcie_bank bank, uint32_t offset, uint32_t value)
{
    struct fake *f = arg;
    assert((offset & 3u) == 0 && offset / 4u < WORDS);
    uint32_t old = f->reg[bank][offset / 4u];
    ++f->writes;
    f->reg[bank][offset / 4u] = value;
    if (bank == XR_SCU && offset == 0x834)
        ++f->reset_writes;
    if ((bank == XR_PMA0 || bank == XR_PMA1) && offset == 0x150 &&
        ((value ^ old) & 0xffffu)) {
        unsigned lane = bank == XR_PMA1;
        assert(f->prepare_seen && !f->released_at && f->cal_starts < 4);
        f->start_order[f->cal_starts++] = 10 * lane +
            ((value & 0xffffu) == 41500 ? 3u : 2u);
        f->samples[lane] = 0;
    }
    if ((bank == XR_PMA0 || bank == XR_PMA1) && offset == 0x158 &&
        (value & 7) == 3 && !(old & 7)) {
        unsigned lane = bank == XR_PMA1;
        unsigned sample = ++f->samples[lane];
        unsigned gen_index = (f->reg[bank][0x150 / 4] & 0xffffu) == 41500 ? 1u : 0u;
        if (sample <= 7) {
            assert((f->reg[bank][0x794 / 4] & 0x7ffu) == sample << 8);
            ++f->coarse[lane][gen_index];
        }
    }
    if (bank == XR_SCU && offset == 0x88 && (value & PERST_MAIN) == PERST_MAIN &&
        !(old & PERST_MAIN)) {
        assert(f->cal_starts == 4 && f->samples[0] == 25 && f->samples[1] == 25);
        assert(f->reg[XR_MAC0][0x100 / 4] == 0x41474147);
        assert(f->reg[XR_MAC1][0x338 / 4] == 0x1018020f);
        assert(!(f->reg[XR_SCU][0x830 / 4] & BIT(27)));
        f->released_at = f->elapsed;
    }
}

static void delay_us(void *arg, unsigned us)
{
    ((struct fake *)arg)->elapsed += us;
}

static int prepare_shared_phy(void *arg)
{
    struct fake *f = arg;
    assert(!(f->reg[XR_SCU][0x88 / 4] & (PERST_MAIN | BIT(16))));
    assert(f->reg[XR_SCU][0x830 / 4] & BIT(27));
    assert(f->reg[XR_SCU][0x9c / 4] == BIT(7));
    assert(f->reg[XR_MAC0][0x15030 / 4] == 0x50500032);
    assert(f->reg[XR_MAC1][0x10044 / 4] == 0x23020133);
    f->prepare_seen = true;
    return f->fail_prepare ? -EIO : 0;
}

static void reset_fake(void)
{
    fake = (struct fake){0};
    fake.reg[XR_SCU][0x88 / 4] = PERST_MAIN | BIT(16) | BIT(7);
    fake.reg[XR_SCU][0x834 / 4] = BIT(11);
    fake.reg[XR_SCU][0x9c / 4] = 0x7800u | BIT(7);
}

int main(void)
{
    struct xr_pcie_io io = {&fake, read32, write32, delay_us, prepare_shared_phy};
    struct xr_pcie_board board = {XR_STOCK_5112, 0, 0, 0xc, 0};
    struct xr_pcie_result result;

    reset_fake();
    assert(xr_stock_pcie_boot_x2(&io, &board, &result) == 0);
    assert(fake.reset_writes == 2 && fake.reads == 3);
    assert(fake.start_order[0] == 3 && fake.start_order[1] == 13 &&
           fake.start_order[2] == 2 && fake.start_order[3] == 12);
    for (unsigned lane = 0; lane < 2; ++lane) {
        assert(result.gen3[lane].idac == 0x7af - lane);
        assert(result.gen2[lane].idac == 0x56d + lane);
        assert(result.gen3[lane].attempts == 10 && result.gen3[lane].last_locked);
        assert(result.gen2[lane].attempts == 10 && result.gen2[lane].last_locked);
        assert(fake.coarse[lane][0] == 7 && fake.coarse[lane][1] == 7);
        enum xr_pcie_bank bank = lane == 0 ? XR_PMA0 : XR_PMA1;
        assert((fake.reg[bank][0x174 / 4] & 0x7ffu) == result.gen2[lane].idac);
        assert(!(fake.reg[bank][0x794 / 4] & BIT(16)));
    }
    assert(fake.reg[XR_SCU][0x834 / 4] == BIT(11));
    printf("PASS: retail x2 sequence, 4 calibration passes, coarse bin 7, both MACs, PERST and link-up\n");

    reset_fake();
    board.image = XR_ENGINEERING;
    fake.link_never_up = fake.lock_never = true;
    assert(xr_stock_pcie_boot_x2(&io, &board, &result) == -ETIMEDOUT);
    assert(fake.reset_writes == 3 && fake.reads == 10001);
    assert(fake.elapsed == fake.released_at + 1100000);
    assert(result.gen3[0].unlocked_samples == 10 && !result.gen3[0].last_locked);
    assert(result.mac_ltssm == 0x01000000);
    printf("PASS: engineering reset variant, per-sample unlock reporting and LINKUP timeout\n");

    reset_fake();
    board.wifi1_mode = 1;
    assert(xr_stock_pcie_boot_x2(&io, &board, &result) == -EOPNOTSUPP);
    assert(fake.writes == 0);
    board.wifi1_mode = 0;
    board.hir = 0xe;
    board.package_id = 5;
    assert(xr_stock_pcie_boot_x2(&io, &board, &result) == -EOPNOTSUPP);
    assert(fake.writes == 0);
    board.hir = 0xc;
    fake.fail_prepare = true;
    assert(xr_stock_pcie_boot_x2(&io, &board, &result) == -EIO);
    assert(fake.prepare_seen && !fake.released_at && !fake.cal_starts);
    printf("PASS: mode/package exclusions and pre-calibration failure keep link held in reset\n");
    return 0;
}
