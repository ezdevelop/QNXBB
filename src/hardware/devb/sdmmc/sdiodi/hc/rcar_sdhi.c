/*
 * $QNXLicenseC:
 * Copyright 2013, QNX Software Systems.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You
 * may not reproduce, modify or distribute this software except in
 * compliance with the License. You may obtain a copy of the License
 * at: http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTIES OF ANY KIND, either express or implied.
 *
 * This file may contain contributions from others, either as
 * contributors under the License or as licensors under other terms.
 * Please review this entire file for other proprietary rights or license
 * notices, as well as the QNX Development Suite License Guide at
 * http://licensing.qnx.com/license-guide/ for other information.
 * $
 */

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <hw/inout.h>
#include <sys/mman.h>
#include <internal.h>
#include <sys/syspage.h>
#include <inttypes.h>

#ifdef SDIO_HC_RCAR_SDHI
#include <arm/r-car.h>
#include <rcar_sdhi.h>

static int rcar_sdhi_reset(sdio_hc_t *hc);

/* Definitions for sampling clocks */
sdhi_scc_t rcar_gen2_scc_taps[] = {
    {
        .clk = 195000000,
        .tap = 0x00000703,
    },
    {
        .clk = 156000000,
        .tap = 0x00000703,
    },
};

static sdhi_cfg_t sdhi_cfg[SDHI_CHANNELS] =
{
    {
        .base           = RCAR_SDHI0_BASE,
        .tx_mid_rid     = RCAR_DREQ_SDHI0_TXI,
        .rx_mid_rid     = RCAR_DREQ_SDHI0_RXI,
        .clock          = 195000000,
        .irq            = IRQ_SDHI0,
        .bus_shift      = 1,
        .taps           = rcar_gen2_scc_taps,
        .taps_num       = sizeof(rcar_gen2_scc_taps),
    },
    {
        .base           = RCAR_SDHI1_BASE,
        .tx_mid_rid     = RCAR_DREQ_SDHI1_TXI,
        .rx_mid_rid     = RCAR_DREQ_SDHI1_RXI,
        .clock          = 195000000,
        .irq            = IRQ_SDHI0,
        .bus_shift      = 1,
        .taps           = rcar_gen2_scc_taps,
        .taps_num       = sizeof(rcar_gen2_scc_taps),
    },
    {
        .base           = RCAR_SDHI2_BASE,
        .tx_mid_rid     = RCAR_DREQ_SDHI2_TXI,
        .rx_mid_rid     = RCAR_DREQ_SDHI2_RXI,
        .clock          = 97500000,
        .irq            = IRQ_SDHI1,
        .bus_shift      = 0,
    },
    {
        .base           = RCAR_SDHI3_BASE,
        .tx_mid_rid     = RCAR_DREQ_SDHI3_TXI,
        .rx_mid_rid     = RCAR_DREQ_SDHI3_RXI,
        .clock          = 97500000,
        .irq            = IRQ_SDHI2,
        .bus_shift      = 0,
    }
};

static int rcar_sdhi_intr_event(sdio_hc_t *hc)
{
    rcar_sdhi_t     *sdhi;
    sdio_cmd_t      *cmd;
    uint16_t        mask1, mask2;
    uint16_t        stat1, stat2;
    int             cs = CS_CMD_INPROG;

    sdhi  = (rcar_sdhi_t *)hc->cs_hdl;

    mask1 = ~rcar_sdh_read(sdhi, SDH_INFO1_MASK);
    mask2 = ~rcar_sdh_read(sdhi, SDH_INFO2_MASK);
    stat1 = rcar_sdh_read(sdhi, SDH_INFO1);
    stat2 = rcar_sdh_read(sdhi, SDH_INFO2);
    stat1 &= mask1;
    stat2 &= mask2;

    /* Clear interrupt status */
    rcar_sdh_write(sdhi, SDH_INFO1, ~stat1);
    rcar_sdh_write(sdhi, SDH_INFO2, ~stat2);

    /*
     * Card insertion and card removal events
     */
    if (stat1 & (SDH_INFO1_INST | SDH_INFO1_RMVL)) {
        /*
         * After insert the card, insert event and then remove event( with both insert and remove bits set) occur.
         * To avoid call sdio_cd 2 times, only process the first insert event.
         */
        if (stat1 & (SDH_INFO1_INST)) {
            if (sdhi->card_insert == 0) {
                sdhi->card_insert = 1;
                sdio_hc_event(hc, HC_EV_CD);
            }
        } else {
            sdhi->card_insert = 0;
            sdio_hc_event(hc, HC_EV_CD);
        }
    }

    // no command ?
    if ((cmd = hc->wspc.cmd) == NULL)
        return (EOK);

    /* Check of errors */
    if (stat2 & (SDH_INFO2_ALL_ERR)) {
        uint16_t    ests1, ests2;

        ests1 = rcar_sdh_read(sdhi, SDH_ERR_STS1);
        ests2 = rcar_sdh_read(sdhi, SDH_ERR_STS2);

        sdio_slogf(_SLOGC_SDIODI, _SLOG_ERROR, hc->cfg.verbosity, 1,
                        "%s, ERROR in HC, CMD%d, %x : %x",
                        __func__, cmd->opcode, rcar_sdh_read(sdhi, SDH_ERR_STS1), rcar_sdh_read(sdhi, SDH_ERR_STS2));

        if (stat2 & SDH_INFO2_DTO)          cs = CS_DATA_TO_ERR;
        if (ests1 & (1 << 10))              cs = CS_DATA_CRC_ERR;
        if (ests1 & ((1 << 8) | (1 << 9)))  cs = CS_CMD_CRC_ERR;
        if (ests1 & (1 << 5))               cs = CS_CMD_END_ERR;
        if (ests1 & (1 << 4))               cs = CS_DATA_END_ERR;
        if (ests1 & ((1 << 1) | (1 << 0)))  cs = CS_CMD_IDX_ERR;
        if (ests2 & (1 << 5))               cs = CS_DATA_CRC_ERR;
        if (ests2 & ((1 << 5) | (1 << 4)))  cs = CS_DATA_TO_ERR;
        if (ests2 & ((1 << 1) | (1 << 0)))  cs = CS_CMD_TO_ERR;
        if (!cs)                            cs = CS_CMD_CMP_ERR;
	} else {
        /* End of command */
        if (stat1 & SDH_INFO1_RE) {
            cs = CS_CMD_CMP;

            if ((cmd->flags & SCF_RSP_136)) {
                uint32_t    *resp = &cmd->rsp[0];

                resp[3] = rcar_sdh_read(sdhi, SDH_RSP00) | (rcar_sdh_read(sdhi, SDH_RSP01) << 16);
                resp[2] = rcar_sdh_read(sdhi, SDH_RSP02) | (rcar_sdh_read(sdhi, SDH_RSP03) << 16);
                resp[1] = rcar_sdh_read(sdhi, SDH_RSP04) | (rcar_sdh_read(sdhi, SDH_RSP05) << 16);
                resp[0] = rcar_sdh_read(sdhi, SDH_RSP06) | (rcar_sdh_read(sdhi, SDH_RSP07) << 16);

                resp[0] = (resp[0] << 8) | (resp[1] >> 24);
                resp[1] = (resp[1] << 8) | (resp[2] >> 24);
                resp[2] = (resp[2] << 8) | (resp[3] >> 24);
                resp[3] = (resp[3] << 8);
            } else if ((cmd->flags & SCF_RSP_PRESENT))
                cmd->rsp[0] = rcar_sdh_read(sdhi, SDH_RSP00) | (rcar_sdh_read(sdhi, SDH_RSP01) << 16);
        } else if (stat1 & SDH_INFO1_AE) {
            /* End of data transfer */
            cs = CS_CMD_CMP;
            cmd->rsp[0] = rcar_sdh_read(sdhi, SDH_RSP00) | (rcar_sdh_read(sdhi, SDH_RSP01) << 16);
        }
    }

    if (stat1 & SDH_INFO1_RMVL)
        cs = CS_CARD_REMOVED;

    if (cs != CS_CMD_INPROG) {
        if (cmd->flags & SCF_CTYPE_ADTC) {
            sdhi->dmafuncs.xfer_complete(sdhi->dma);

            /* The SD_BUF read/write DMA transfer is disabled */
            rcar_sdh_write(sdhi, SDH_CC_EXT_MODE, 0);
        }
        sdio_cmd_cmplt(hc, cmd, cs);
    }

    return (EOK);
}

static int rcar_sdhi_event(sdio_hc_t *hc, sdio_event_t *ev)
{
    rcar_sdhi_t *sdhi = (rcar_sdhi_t *)hc->cs_hdl;
    int         status = CS_CMD_INPROG;

    switch (ev->code) {
        case HC_EV_INTR:
            status = rcar_sdhi_intr_event(hc);
            InterruptUnmask(sdhi->irq, hc->hc_iid);
            break;

        default:
            break;
    }

    return (status);
}

static int rcar_get_xfer_size(rcar_sdhi_t *sdhi, sdio_cmd_t *cmd)
{
    if (cmd->blksz != sdhi->blksz) {
        sdhi->blksz = cmd->blksz;
        if (sdhi->rev == RCAR_SDHI_VER_490C)
            rcar_sdh_write(sdhi, SDH_DMACR_490C, cmd->blksz >= 32 ? 0xA000 : 0);
        else
            rcar_sdh_write(sdhi, SDH_DMACR_CB0D, cmd->blksz >= 32 ? 0x0004 : 0);
    }

    return (cmd->blksz >= 32 ? 32 : 4);
}

static int rcar_sdhi_dma_setup(sdio_hc_t *hc, sdio_cmd_t *cmd)
{
    rcar_sdhi_t     *sdhi;
    dma_transfer_t  tinfo;
    dma_addr_t      saddr, daddr;
    sdio_sge_t      *sgp;
    dma_addr_t      *sgaddr;
    int             sgc, sgi;

    sdhi = (rcar_sdhi_t *)hc->cs_hdl;

    sgc = cmd->sgc;
    sgp = cmd->sgl;

    if (!(cmd->flags & SCF_DATA_PHYS)) {
        sdio_vtop_sg(sgp, sdhi->sgl, sgc, cmd->mhdl);
        sgp = sdhi->sgl;
    }

    /* The SD_BUF read/write DMA transfer is enabled */
    rcar_sdh_write(sdhi, SDH_CC_EXT_MODE, SDH_CC_EXT_DMASDRW);
    memset(&tinfo, 0, sizeof(tinfo));

    tinfo.xfer_unit_size = rcar_get_xfer_size(sdhi, cmd);
    tinfo.xfer_bytes     = cmd->blksz * cmd->blks;

    if (cmd->flags & SCF_DIR_IN) {
        saddr.paddr      = sdhi->pbase + (SDH_BUF0 << sdhi->bus_shift) + 0x2000;
        tinfo.src_addrs  = &saddr;

        if (sgc > 1) {
            sgaddr = alloca(sgc * sizeof(dma_addr_t));

            tinfo.dst_flags    |= DMA_ADDR_FLAG_SEGMENTED;
            tinfo.dst_fragments = sgc;
            tinfo.dst_addrs     = sgaddr;
            for (sgi = 0; sgi < sgc; sgi++, sgaddr++, sgp++) {
                sgaddr->paddr   = sgp->sg_address;
                sgaddr->len     = sgp->sg_count;
            }
        } else {
            daddr.paddr         = sgp->sg_address;
            tinfo.dst_addrs     = &daddr;
        }
        tinfo.src_flags  = DMA_ADDR_FLAG_NO_INCREMENT | DMA_ADDR_FLAG_DEVICE;
        tinfo.req_id     = sdhi->rx_mid_rid;    // Receive DMA request ID
    } else {
        if (sgc > 1) {
            sgaddr = alloca(sgc * sizeof(dma_addr_t));

            tinfo.src_flags    |= DMA_ADDR_FLAG_SEGMENTED;
            tinfo.src_fragments = sgc;
            tinfo.src_addrs     = sgaddr;
            for (sgi = 0; sgi < sgc; sgi++, sgaddr++, sgp++) {
                sgaddr->paddr   = sgp->sg_address;
                sgaddr->len     = sgp->sg_count;
            }
        } else {
            saddr.paddr         = sgp->sg_address;
            tinfo.src_addrs     = &saddr;
        }

        daddr.paddr      = sdhi->pbase + (SDH_BUF0 << sdhi->bus_shift);
        tinfo.dst_addrs  = &daddr;
        tinfo.dst_flags  = DMA_ADDR_FLAG_NO_INCREMENT | DMA_ADDR_FLAG_DEVICE;
        tinfo.req_id     = sdhi->tx_mid_rid;    // Trasmit DMA request ID
    }
    sdhi->dmafuncs.setup_xfer(sdhi->dma, &tinfo);
    sdhi->dmafuncs.xfer_start(sdhi->dma);

    return (EOK);
}

static int rcar_sdhi_xfer_setup(sdio_hc_t *hc, sdio_cmd_t *cmd)
{
    rcar_sdhi_t *sdhi;
    int         status = EOK;

    sdhi = (rcar_sdhi_t *)hc->cs_hdl;

    status = sdio_sg_start(hc, cmd->sgl, cmd->sgc);

    if (cmd->sgc && (hc->caps & HC_CAP_DMA)) {
        if ((status = rcar_sdhi_dma_setup(hc, cmd)) == EOK) {
        }
    }

    // block size
    rcar_sdh_write(sdhi, SDH_SIZE, cmd->blksz);

    /* only valid for multi-block transfer */
    if (cmd->blks > 1)
        rcar_sdh_write(sdhi, SDH_SECCNT, cmd->blks);

    return (status);
}

static int rcar_sdhi_cmd(sdio_hc_t *hc, sdio_cmd_t *cmd)
{
    rcar_sdhi_t     *sdhi;
    uint16_t        command;
    int             status, i;

    sdhi = (rcar_sdhi_t *)hc->cs_hdl;

    for (i = 0; i < SDHI_TMOUT; i++) {
        status = rcar_sdh_read(sdhi, SDH_INFO2);
        if (!(status & SDH_INFO2_CBSY) && (status & SDH_INFO2_SCLKDIVEN))
            break;
        nanospin_ns(1000);
    }

    if (i >= SDHI_TMOUT) {
        sdio_slogf(_SLOGC_SDIODI, _SLOG_ERROR, hc->cfg.verbosity, 1,
            "%s: SD: CMD%d cannot execute because BUS busy", __func__, cmd->opcode);
        return (EAGAIN);
    }

    /* Clear Status */
    rcar_sdh_write(sdhi, SDH_INFO1, rcar_sdh_read(sdhi, SDH_INFO1) & ~(SDH_INFO1_AE | SDH_INFO1_RE));
    rcar_sdh_write(sdhi, SDH_INFO2, rcar_sdh_read(sdhi, SDH_INFO2) & ~SDH_INFO2_ALL_ERR);

    command = cmd->opcode;

    switch (cmd->flags & (0x1F << 4)) {
        case SCF_RSP_NONE:
            command |= SDH_CMD_NORSP;
            break;
        case SCF_RSP_R1:
            command |= SDH_CMD_RSPR1;
            break;
        case SCF_RSP_R1B:
            command |= SDH_CMD_RSPR1B;
            break;
        case SCF_RSP_R2:
            command |= SDH_CMD_RSPR2;
            break;
        case SCF_RSP_R3:
            command |= SDH_CMD_RSPR3;
            break;
    }

    if (cmd->flags & SCF_CTYPE_ADTC) {
        if ((status = rcar_sdhi_xfer_setup(hc, cmd)) != EOK)
            return (status);

        command |= SDH_CMD_ADTC;
        if (cmd->flags & SCF_DIR_IN)
            command |= SDH_CMD_DAT_READ;
        if (cmd->flags & SCF_MULTIBLK) {
            rcar_sdh_write(sdhi, SDH_STOP, SDH_STOP_SEC);
            command |= SDH_CMD_DAT_MULTI;
            if (!(hc->caps & HC_CAP_ACMD12))
                command |= SDH_CMD_NOAC12;
        }

        // card insertion/removal are always enabled
        rcar_sdh_write(sdhi, SDH_INFO1_MASK, ~(SDH_INFO1_AE | SDH_INFO1_RMVL | SDH_INFO1_INST));
    } else
        rcar_sdh_write(sdhi, SDH_INFO1_MASK, ~(SDH_INFO1_RE | SDH_INFO1_RMVL | SDH_INFO1_INST));

    rcar_sdh_write(sdhi, SDH_INFO2_MASK, ~SDH_INFO2_ALL_ERR);

    rcar_sdh_write(sdhi, SDH_ARG0, cmd->arg);
    rcar_sdh_write(sdhi, SDH_ARG1, cmd->arg >> 16);

    rcar_sdh_write(sdhi, SDH_CMD, command);

    return (EOK);
}

static int rcar_sdhi_abort(sdio_hc_t *hc, sdio_cmd_t *cmd)
{
    return (EOK);
}

static int rcar_sdhi_pwr(sdio_hc_t *hc, int vdd)
{
    rcar_sdhi_reset(hc);

    hc->vdd = vdd;

    return (EOK);
}

static int rcar_sdhi_bus_mode(sdio_hc_t *hc, int bus_mode)
{
    hc->bus_mode = bus_mode;

    return (EOK);
}

static int rcar_sdhi_bus_width(sdio_hc_t *hc, int width)
{
    rcar_sdhi_t *sdhi;
    uint32_t    hctl;

    sdhi = (rcar_sdhi_t *)hc->cs_hdl;

    hctl = rcar_sdh_read(sdhi, SDH_OPTION);

    if (width == 4)
        hctl &= ~(SDH_OPTION_WIDTH_1);
    else
        hctl |= (SDH_OPTION_WIDTH_1);

    hctl &= 0xFFF0;

    rcar_sdh_write(sdhi, SDH_OPTION, hctl);

    hc->bus_width = width;

    return (EOK);
}

static int rcar_sdhi_clk(sdio_hc_t *hc, int clk)
{
    rcar_sdhi_t *sdhi;
    int         clkctl;
    uint32_t    clock;
    uint16_t    info2;
    int         cnt;

    sdhi = (rcar_sdhi_t *)hc->cs_hdl;

    for (cnt = SDHI_TMOUT; cnt >= 0; --cnt) {
        info2 = rcar_sdh_read(sdhi, SDH_INFO2);
        if (!(info2 & SDH_INFO2_CBSY) && (info2 & SDH_INFO2_SCLKDIVEN))
            break;

        nanospin_ns(1000);
    }
    if (cnt <= 0) {
        sdio_slogf(_SLOGC_SDIODI, _SLOG_INFO, hc->cfg.verbosity, 1, "%s: Busy state! Cannot change the clock!", __func__);
        return (EAGAIN);
    }

    /* stop clock */
    rcar_sdh_write(sdhi, SDH_CLK_CTRL, rcar_sdh_read(sdhi, SDH_CLK_CTRL) & ~SDH_CLKCTRL_SCLKEN);

    if (clk > hc->clk_max)
        clk = hc->clk_max;

    clock = sdhi->pclk;

    for (clkctl = 0; clkctl < 8; clkctl++, clock >>= 1) {
        if (clock <= clk)
            break;
    }

    // clkctl = 1 will give the result 0, which is what we wanted
    rcar_sdh_write(sdhi, SDH_CLK_CTRL, clkctl == 0 ? 0xFF : 1 << (clkctl - 1));

    while ((rcar_sdh_read(sdhi, SDH_INFO2) & SDH_INFO2_SCLKDIVEN) == 0)
        ;

    rcar_sdh_write(sdhi, SDH_CLK_CTRL, rcar_sdh_read(sdhi, SDH_CLK_CTRL) | SDH_CLKCTRL_SCLKEN);

    hc->clk = sdhi->busclk = clock;

    return (EOK);
}

static int rcar_sdhi_signal_voltage(sdio_hc_t *hc, int signal_voltage)
{
    hc->signal_voltage = signal_voltage;

    return (EOK);
}

static int rcar_drv_type(sdio_hc_t *hc, int type)
{
    hc->drv_type = type;

    return (EOK);
}

static int rcar_sdhi_timing(sdio_hc_t *hc, int timing)
{
    hc->timing = timing;

    //rcar_sdhi_clk(hc, hc->clk);

    return (EOK);
}

static int rcar_sdhi_init_tuning(sdio_hc_t *hc)
{
    sdio_hc_cfg_t   *cfg;
    rcar_sdhi_t     *sdhi;
    uintptr_t       base;
    sdhi_scc_t      *taps;
    int             scc_tapnum;
    int             taps_num;
    int             i;
#ifdef RCARH2_ES1_SDHI_WORKAROUND
    uintptr_t       product_reg;
#endif

    sdhi        = (rcar_sdhi_t *)hc->cs_hdl;
    base        = sdhi->base;
    cfg         = &hc->cfg;
    scc_tapnum  = 8;

    /* set sampling clock selection range */
    if (scc_tapnum) {
#ifdef RCARH2_ES1_SDHI_WORKAROUND
        if ((product_reg = mmap_device_io(4, PRODUCT_REGISTER)) == (uintptr_t)MAP_FAILED) {
            sdio_slogf(_SLOGC_SDIODI, _SLOG_ERROR, hc->cfg.verbosity, 1,
                    "%s: Cannot mmap_device_io (0x%x) %s", __func__, PRODUCT_REGISTER, strerror(errno));
            return 0;
        }

        if ((in32(product_reg) & PRODUCT_CUT_MASK) == PRODUCT_H2_BIT)
            scc_tapnum = 10;

        munmap_device_io(product_reg, 4);
#endif
        out32(base + SDH_SCC_DTCNTL, scc_tapnum << 16);
    }

    /* Initialize SCC */
    rcar_sdh_write(sdhi, SDH_INFO1, 0x0000);
    rcar_sdh_write(sdhi, SDH_INFO2, 0x0000);

    out32(base + SDH_SCC_DTCNTL, RCAR_SDHI_SCC_DTCNTL_TAPEN | in32(base + SDH_SCC_DTCNTL));

    rcar_sdh_write(sdhi, SDH_CLK_CTRL, rcar_sdh_read(sdhi, SDH_CLK_CTRL) & ~(1 << 8));

    out32(base + SDH_SCC_CKSEL, RCAR_SDHI_SCC_CKSEL_DTSEL | in32(base + SDH_SCC_CKSEL));

    rcar_sdh_write(sdhi, SDH_CLK_CTRL, rcar_sdh_read(sdhi, SDH_CLK_CTRL) | (1 << 8));

    out32(base + SDH_SCC_RVSCNTL, ~RCAR_SDHI_SCC_RVSCNTL_RVSEN & in32(base + SDH_SCC_RVSCNTL));

    taps     = sdhi_cfg[cfg->idx].taps;
    taps_num = sdhi_cfg[cfg->idx].taps_num;
    for (i = 0; i < taps_num; i++, taps++) {
        if (taps->clk == hc->clk_max) {
            out32(base + SDH_SCC_DT2FF, taps->tap);
            break;
        }
    }

    if (taps->clk != hc->clk_max)
        sdio_slogf(_SLOGC_SDIODI, _SLOG_ERROR, hc->cfg.verbosity, 1, "%s : Unknown clk_max (%d) for SDR104", __func__, hc->clk_max);

    /* Read TAPNUM */
    taps_num = (in32(base + SDH_SCC_DTCNTL) >> 16) & 0xf;

    return (taps_num);
}

static int rcar_sdhi_prepare_tuning(sdio_hc_t *hc, uint32_t tap)
{
    rcar_sdhi_t *sdhi;

    sdhi = (rcar_sdhi_t *)hc->cs_hdl;

    /* Set sampling clock position */
    out32(sdhi->base + SDH_SCC_TAPSET, tap);

    return (EOK);
}

static int rcar_sdhi_select_tuning(sdio_hc_t *hc, int *tap)
{
    rcar_sdhi_t *sdhi;
    uintptr_t   base;
    uint32_t    tap_num;    /* total number of taps */
    uint32_t    tap_cnt;    /* counter of tuning success */
    uint32_t    tap_set;    /* tap position */
    uint32_t    tap_start;  /* start position of tuning success */
    uint32_t    tap_end;    /* end position of tuning success */
    uint32_t    ntap;       /* temporary counter of tuning success */
    int         i;

    sdhi = (rcar_sdhi_t *)hc->cs_hdl;
    base = sdhi->base;

    /* Clear SCC_RVSREQ */
    out32(base + SDH_SCC_RVSREQ, 0x00000000);

    /* Select SCC */
    tap_num = (in32(base + SDH_SCC_DTCNTL) >> 16) & 0xf;

    tap_cnt   = 0;
    ntap      = 0;
    tap_start = 0;
    tap_end   = 0;

    for (i = 0; i < tap_num * 2; i++) {
        if (tap[i] == 0)
            ntap++;
        else {
            if (ntap > tap_cnt) {
                tap_start = i - ntap;
                tap_end = i - 1;
                tap_cnt = ntap;
            }
            ntap = 0;
        }
    }

    if (ntap > tap_cnt) {
        tap_start = i - ntap;
        tap_end = i - 1;
        tap_cnt = ntap;
    }

    if (tap_cnt >= RCAR_SDHI_MAX_TAP)
        tap_set = (tap_start + tap_end) / 2 % tap_num;
    else
        return (EIO);

    /* Set SCC */
    out32(base + SDH_SCC_TAPSET, tap_set);

    /* Enable auto re-tuning */
    out32(base + SDH_SCC_RVSCNTL, RCAR_SDHI_SCC_RVSCNTL_RVSEN | in32(base + SDH_SCC_RVSCNTL));

    return (EOK);
}

static void rcar_sdhi_scc_reset(sdio_hc_t *hc)
{
    rcar_sdhi_t *sdhi;
    uintptr_t   base;

    sdhi = (rcar_sdhi_t *)hc->cs_hdl;
    base = sdhi->base;

    if (sdhi->flags & RCAR_SDHI_HAS_UHS_SCC) {
        /* Reset SCC */
        rcar_sdh_write(sdhi, SDH_CLK_CTRL, rcar_sdh_read(sdhi, SDH_CLK_CTRL) & ~0x0100);

        out32(base + SDH_SCC_CKSEL, ~RCAR_SDHI_SCC_CKSEL_DTSEL & in32(base + SDH_SCC_CKSEL));

        rcar_sdh_write(sdhi, SDH_CLK_CTRL, rcar_sdh_read(sdhi, SDH_CLK_CTRL) | 0x0100);

        out32(base + SDH_SCC_DTCNTL, ~RCAR_SDHI_SCC_DTCNTL_TAPEN & in32(base + SDH_SCC_DTCNTL));

        out32(base + SDH_SCC_RVSCNTL, ~RCAR_SDHI_SCC_RVSCNTL_RVSEN & in32(base + SDH_SCC_RVSCNTL));
    }
}

static int rcar_sdhi_tune(sdio_hc_t *hc, int op)
{
    struct sdio_cmd *cmd;
    sdio_sge_t      sge;
    uint32_t        *td;
    int             tap[RCAR_SDHI_TUNING_RETRIES];
    int             tlc;
    int             tlen;
    int             status;
    int             val;
    int             tap_num;
    int             timeout;

    /* return if not HS200 or SDR104, and not SDR50 that requires tuning */
    if ((hc->timing != TIMING_SDR104) && (hc->timing != TIMING_HS200) && ((hc->timing == TIMING_SDR50)))
        return (EOK);

    val     = 0;
    tap_num = rcar_sdhi_init_tuning(hc);
    tlen    = 64;

    if ((cmd = sdio_alloc_cmd()) == NULL)
        return (ENOMEM);

    /* on RCAR the driver has to read the tuning data and compare it with the reference data
     * therefore we need a buffer here for the tuning data */
    if ((td = (uint32_t *)sdio_alloc(tlen)) == NULL) {
        sdio_free_cmd(cmd);
        return (ENOMEM);
    }

    tlc     = RCAR_SDHI_TUNING_RETRIES;
    timeout = RCAR_SDHI_TUNING_TIMEOUT;

    do {
        /* clear tuning data buffer to avoid comparing old data after unsuccessful transfer */
        memset(td, 0, tlen);

        rcar_sdhi_prepare_tuning(hc, val % tap_num);

        if (!tlc && !timeout)
            break;

        /* setup tuning command */
        sdio_setup_cmd(cmd, SCF_CTYPE_ADTC | SCF_RSP_R1, op, 0);
        sge.sg_count = tlen; sge.sg_address = (paddr_t)td;
        sdio_setup_cmd_io(cmd, SCF_DIR_IN, 1, tlen, &sge, 1, NULL);

        if ((status = sdio_issue_cmd(&hc->device, cmd, SDHI_TMOUT)))
            break;

        /* determine largest timing window where data transfer is working */
        if ((cmd->status != CS_CMD_CMP))
            tap[val] = -1;
        else
            tap[val] = 0;

        val++;
        timeout--;
        tlc--;
        delay(1);
    } while ((val < (tap_num * 2)) && (tlc || timeout));

    if (tlc || timeout) {
        status = rcar_sdhi_select_tuning(hc, tap);
    } else {
        sdio_slogf(_SLOGC_SDIODI, _SLOG_ERROR, hc->cfg.verbosity, 1, "%s: Tuning procedure failed", __func__);
        status = EIO;
    }

    sdio_free(td, tlen);
    sdio_free_cmd(cmd);

    if (status != EOK)
        rcar_sdhi_scc_reset(hc);

    return (status);
}

static int rcar_sdhi_cd(sdio_hc_t *hc)
{
    rcar_sdhi_t *sdhi;
    int         cstate, pstate;

    sdhi   = (rcar_sdhi_t *)hc->cs_hdl;
    cstate = CD_RMV;
    pstate = rcar_sdh_read(sdhi, SDH_INFO1);

    hc->caps |= HC_CAP_CD_INTR;

    if ((pstate & SDH_INFO1_CD)) {
        cstate  |= CD_INS;
        if (pstate & SDH_INFO1_WP)
            cstate |= CD_WP;
    }

    return (cstate);
}

int rcar_sdhi_dinit(sdio_hc_t *hc)
{
    rcar_sdhi_t *sdhi;

    if (!hc || !hc->cs_hdl)
        return (EOK);

    sdhi = (rcar_sdhi_t *)hc->cs_hdl;

    if (sdhi->base) {
        if (hc->hc_iid != -1)
            InterruptDetach(hc->hc_iid);

        munmap_device_io(sdhi->base, RCAR_SDHI_SIZE);
    }

    if (sdhi->dma) {
        sdhi->dmafuncs.channel_release(sdhi->dma);
        sdhi->dmafuncs.fini();
    }

    free(sdhi);
    hc->cs_hdl = NULL;

    return (EOK);
}

static int rcar_sdhi_reset(sdio_hc_t *hc)
{
    rcar_sdhi_t *sdhi = (rcar_sdhi_t *)hc->cs_hdl;

    rcar_sdh_write(sdhi, SDH_SOFT_RST, 0);
    delay(1);
    rcar_sdh_write(sdhi, SDH_SOFT_RST, 1);
    delay(1);

    rcar_sdh_write(sdhi, SDH_OPTION, 0x80EE);
    rcar_sdh_write(sdhi, SDH_INFO1_MASK, 0xFFFF);
    rcar_sdh_write(sdhi, SDH_INFO2_MASK, 0xFFFF);

    rcar_sdh_write(sdhi, SDH_INFO1_MASK, ~(SDH_INFO1_INST | SDH_INFO1_RMVL));

    if (sdhi->busclk == 0)
        sdhi->busclk = 400 * 1000;

    rcar_sdhi_clk(hc, sdhi->busclk);

    // Enable 32 bit DMA access
    rcar_sdh_write(sdhi, SDH_EXT_ACC, sdhi->rev == RCAR_SDHI_VER_490C ? 1 : 0);
    sdhi->blksz = 0;

    return (EOK);
}

static sdio_hc_entry_t rcar_sdhi_entry = {
    16,
    rcar_sdhi_dinit, NULL,
    rcar_sdhi_cmd, rcar_sdhi_abort,
    rcar_sdhi_event, rcar_sdhi_cd, rcar_sdhi_pwr,
    rcar_sdhi_clk, rcar_sdhi_bus_mode,
    rcar_sdhi_bus_width, rcar_sdhi_timing,
    rcar_sdhi_signal_voltage, rcar_drv_type, NULL, rcar_sdhi_tune, NULL
};

static int rcar_sdhi_init_dma(sdio_hc_t *hc)
{
    sdio_hc_cfg_t       *cfg = &hc->cfg;
    rcar_sdhi_t         *sdhi = (rcar_sdhi_t *)hc->cs_hdl;
    dma_channel_query_t query;
    dma_channel_info_t  info;

    if (get_dmafuncs(&sdhi->dmafuncs, sizeof(sdhi->dmafuncs)) == -1) {
        sdio_slogf(_SLOGC_SDIODI, _SLOG_ERROR, hc->cfg.verbosity, 1, "%s: Failed to get DMA lib functions", __func__);
        return ENODEV;
    }

    sdhi->dmafuncs.init(NULL);
    if ((sdhi->dma = sdhi->dmafuncs.channel_attach(NULL, NULL, NULL, 0, DMA_ATTACH_ANY_CHANNEL)) == NULL) {
        sdio_slogf(_SLOGC_SDIODI, _SLOG_ERROR, hc->cfg.verbosity, 1, "%s: Unable to attach to Rx DMA Channel", __func__);
        return ENODEV;
    }

    sdhi->dmafuncs.query_channel(sdhi->dma, &query);
    sdhi->dmafuncs.channel_info(query.chan_idx, &info);

    cfg->sg_max = info.max_src_segments > 1 ?  info.max_src_segments : 1;

    return (EOK);
}

int rcar_sdhi_init(sdio_hc_t *hc)
{
    sdio_hc_cfg_t   *cfg;
    rcar_sdhi_t     *sdhi;
    uintptr_t       base;
    struct sigevent event;
#ifdef RCARH2_ES1_SDHI_WORKAROUND
    uintptr_t       product_reg;
#endif

    hc->hc_iid  = -1;
    cfg         = &hc->cfg;

    memcpy(&hc->entry, &rcar_sdhi_entry, sizeof(sdio_hc_entry_t));

    if ((sdhi = hc->cs_hdl = calloc(1, sizeof(rcar_sdhi_t))) == NULL)
        return (ENOMEM);

    if (cfg->base_addrs > 0 && cfg->dma_chnls > 0 && cfg->irqs > 0) {
        sdhi->pbase      = cfg->base_addr[0];
        sdhi->bus_shift  = cfg->base_addr_size[0];
        sdhi->tx_mid_rid = cfg->dma_chnl[0];
        sdhi->rx_mid_rid = cfg->dma_chnls > 1 ? cfg->dma_chnl[1] : sdhi->tx_mid_rid + 1;
        sdhi->irq        = cfg->irq[0];
        sdhi->pclk       = cfg->clk > 0 ? cfg->clk : 97500000;
    } else if (cfg->idx >= 0 && cfg->idx < SDHI_CHANNELS) {
        sdhi->pbase      = sdhi_cfg[cfg->idx].base;
        sdhi->bus_shift  = sdhi_cfg[cfg->idx].bus_shift;
        sdhi->tx_mid_rid = sdhi_cfg[cfg->idx].tx_mid_rid;
        sdhi->rx_mid_rid = sdhi_cfg[cfg->idx].rx_mid_rid;
        sdhi->irq        = sdhi_cfg[cfg->idx].irq;
        sdhi->pclk       = cfg->clk > 0 ? cfg->clk : sdhi_cfg[cfg->idx].clock;
    } else {
        rcar_sdhi_dinit(hc);
        return (ENODEV);
    }

#ifdef RCARH2_ES1_SDHI_WORKAROUND
    if (sdhi->pclk) {
        if ((product_reg = mmap_device_io(4, PRODUCT_REGISTER)) == (uintptr_t)MAP_FAILED) {
            sdio_slogf(_SLOGC_SDIODI, _SLOG_ERROR, hc->cfg.verbosity, 1,
                    "%s: cannot mmap_device_io (0x%x) %s", __func__, PRODUCT_REGISTER, strerror(errno));
            rcar_sdhi_dinit(hc);
            return (errno);
        }

        if ((in32(product_reg) & PRODUCT_CUT_MASK) == PRODUCT_H2_BIT)
            if (sdhi->pclk > 156000000)
                sdhi->pclk = 156000000;

        munmap_device_io(product_reg, 4);
    }
#endif

    if ((base = sdhi->base = (uintptr_t)mmap_device_io(RCAR_SDHI_SIZE, sdhi->pbase)) == (uintptr_t)MAP_FAILED) {
        sdio_slogf(_SLOGC_SDIODI, _SLOG_ERROR, hc->cfg.verbosity, 1,
            "%s: SDHI base mmap_device_io (0x%x) %s", __func__, sdhi->pbase, strerror(errno));
        rcar_sdhi_dinit(hc);
        return (errno);
    }

    sdhi->rev = rcar_sdh_read(sdhi, SDH_VERSION);
    if (sdhi->rev != RCAR_SDHI_VER_490C && sdhi->rev != RCAR_SDHI_VER_CB0D) {
        sdio_slogf(_SLOGC_SDIODI, _SLOG_ERROR, hc->cfg.verbosity, 1, "%s: Unknown SDHI revision %x", __func__, sdhi->rev);
        rcar_sdhi_dinit(hc);
        return (errno);
    }

    hc->clk_max = sdhi->pclk;

    hc->caps |= HC_CAP_BSY | HC_CAP_BW4; // | HC_CAP_CD_INTR;
    hc->caps |= HC_CAP_ACMD12;
    hc->caps |= HC_CAP_HS;
    hc->caps |= HC_CAP_DMA;
    hc->caps |= HC_CAP_SV_1_8V | HC_CAP_SV_3_3V;
    hc->caps |= HC_CAP_XPC_1_8V | HC_CAP_XPC_3_3V;

    hc->ocr   = OCR_VDD_17_195 | OCR_VDD_32_33 | OCR_VDD_33_34;

    hc->caps |= HC_CAP_SDR50;
    if ((cfg->idx == 0) || (cfg->idx == 1)) {
        hc->caps |= HC_CAP_SDR104;
        sdhi->flags |= RCAR_SDHI_HAS_UHS_SCC;
    }

    hc->caps &= cfg->caps;      /* reconcile command line options */

    if (rcar_sdhi_init_dma(hc) != EOK) {
        rcar_sdhi_dinit(hc);
        return (ENODEV);
    }

    //rcar_sdhi_reset(hc);

    // we don't want this interrupt at the driver startup
    while (rcar_sdh_read(sdhi, SDH_INFO1) & (SDH_INFO1_INST | SDH_INFO1_RMVL))
        rcar_sdh_write(sdhi, SDH_INFO1, ~(SDH_INFO1_INST | SDH_INFO1_RMVL));

    rcar_sdh_write(sdhi, SDH_INFO1_MASK, ~(SDH_INFO1_INST | SDH_INFO1_RMVL));

    SIGEV_PULSE_INIT(&event, hc->hc_coid, SDIO_PRIORITY, HC_EV_INTR, NULL);
    if ((hc->hc_iid = InterruptAttachEvent(sdhi->irq, &event, _NTO_INTR_FLAGS_TRK_MSK)) == -1) {
        rcar_sdhi_dinit(hc);
        return (errno);
    }

    return (EOK);
}

#endif


#if defined(__QNXNTO__) && defined(__USESRCVERSION)
#include <sys/srcversion.h>
__SRCVERSION("$URL$ $Rev$")
#endif
