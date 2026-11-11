#ifndef XR1710G_STOCK_PCIE_H
#define XR1710G_STOCK_PCIE_H

#include <stdbool.h>
#include <stdint.h>

enum xr_pcie_bank { XR_SCU, XR_MAC0, XR_MAC1, XR_CSR_2L, XR_PMA0, XR_PMA1 };
enum xr_stock_image { XR_STOCK_5112, XR_ENGINEERING };

struct xr_pcie_io {
    void *ctx;
    uint32_t (*read32)(void *, enum xr_pcie_bank, uint32_t offset);
    void (*write32)(void *, enum xr_pcie_bank, uint32_t offset, uint32_t value);
    void (*delay_us)(void *, unsigned us);
    /* Required boundary for the untranscribed first part of L0L1_init. */
    int (*prepare_shared_phy)(void *);
};

struct xr_pcie_board {
    enum xr_stock_image image;
    unsigned wifi0_mode, wifi1_mode; /* stock get_serdes_interface_sel(2/3) */
    unsigned hir, package_id;
};

struct xr_cal_result {
    unsigned idac, fl_out, attempts, unlocked_samples;
    bool last_locked;
};

struct xr_pcie_result {
    struct xr_cal_result gen3[2], gen2[2];
    uint32_t mac_link_status, mac_ltssm;
};

/* Main cold-boot x2 path. Zero means LINKUP, not that Gen3 x2 was negotiated. */
int xr_stock_pcie_boot_x2(const struct xr_pcie_io *, const struct xr_pcie_board *,
                         struct xr_pcie_result *);

#endif
