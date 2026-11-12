
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>

#include "dma.h"
#include "snes.h"
#include "../psp/psp_log.h"

extern const uint8_t *g_rom;  /* sm_rtl.c: the 4 MB LoROM image the game code reads through RomPtr() */

static const int bAdrOffsets[8][4] = {
  {0, 0, 0, 0},
  {0, 1, 0, 1},
  {0, 0, 0, 0},
  {0, 0, 1, 1},
  {0, 1, 2, 3},
  {0, 1, 0, 1},
  {0, 0, 0, 0},
  {0, 0, 1, 1}
};

static const int transferLength[8] = {
  1, 2, 2, 4, 4, 4, 2, 4
};

static void dma_transferByte(Dma* dma, uint16_t aAdr, uint8_t aBank, uint8_t bAdr, bool fromB);

Dma* dma_init(Snes* snes) {
  Dma* dma = malloc(sizeof(Dma));
  dma->snes = snes;
  return dma;
}

void dma_free(Dma* dma) {
  free(dma);
}

void dma_reset(Dma* dma) {
  for(int i = 0; i < 8; i++) {
    dma->channel[i].bAdr = 0xff;
    dma->channel[i].aAdr = 0xffff;
    dma->channel[i].aBank = 0xff;
    dma->channel[i].size = 0xffff;
    dma->channel[i].indBank = 0xff;
    dma->channel[i].tableAdr = 0xffff;
    dma->channel[i].repCount = 0xff;
    dma->channel[i].unusedByte = 0xff;
    dma->channel[i].dmaActive = false;
    dma->channel[i].hdmaActive = false;
    dma->channel[i].mode = 7;
    dma->channel[i].fixed = true;
    dma->channel[i].decrement = true;
    dma->channel[i].indirect = true;
    dma->channel[i].fromB = true;
    dma->channel[i].unusedBit = true;
    dma->channel[i].doTransfer = false;
    dma->channel[i].terminated = false;
    dma->channel[i].offIndex = 0;
  }
  dma->hdmaTimer = 0;
  dma->dmaTimer = 0;
  dma->dmaBusy = false;
}

void dma_saveload(Dma *dma, SaveLoadFunc *func, void *ctx) {
  func(ctx, &dma->channel, offsetof(Dma, pad) + 7 - offsetof(Dma, channel));
}

uint8_t dma_read(Dma* dma, uint16_t adr) {
  uint8_t c = (adr & 0x70) >> 4;
  switch(adr & 0xf) {
    case 0x0: {
      uint8_t val = dma->channel[c].mode;
      val |= dma->channel[c].fixed << 3;
      val |= dma->channel[c].decrement << 4;
      val |= dma->channel[c].unusedBit << 5;
      val |= dma->channel[c].indirect << 6;
      val |= dma->channel[c].fromB << 7;
      return val;
    }
    case 0x1: {
      return dma->channel[c].bAdr;
    }
    case 0x2: {
      return dma->channel[c].aAdr & 0xff;
    }
    case 0x3: {
      return dma->channel[c].aAdr >> 8;
    }
    case 0x4: {
      return dma->channel[c].aBank;
    }
    case 0x5: {
      return dma->channel[c].size & 0xff;
    }
    case 0x6: {
      return dma->channel[c].size >> 8;
    }
    case 0x7: {
      return dma->channel[c].indBank;
    }
    case 0x8: {
      return dma->channel[c].tableAdr & 0xff;
    }
    case 0x9: {
      return dma->channel[c].tableAdr >> 8;
    }
    case 0xa: {
      return dma->channel[c].repCount;
    }
    case 0xb:
    case 0xf: {
      return dma->channel[c].unusedByte;
    }
    default: {
      assert(0);
      return dma->snes->openBus;
    }
  }
}

void dma_write(Dma* dma, uint16_t adr, uint8_t val) {
  uint8_t c = (adr & 0x70) >> 4;
  switch(adr & 0xf) {
    case 0x0: {
      dma->channel[c].mode = val & 0x7;
      dma->channel[c].fixed = val & 0x8;
      dma->channel[c].decrement = val & 0x10;
      dma->channel[c].unusedBit = val & 0x20;
      dma->channel[c].indirect = val & 0x40;
      dma->channel[c].fromB = val & 0x80;
      break;
    }
    case 0x1: {
      dma->channel[c].bAdr = val;
      break;
    }
    case 0x2: {
      dma->channel[c].aAdr = (dma->channel[c].aAdr & 0xff00) | val;
      break;
    }
    case 0x3: {
      dma->channel[c].aAdr = (dma->channel[c].aAdr & 0xff) | (val << 8);
      break;
    }
    case 0x4: {
      dma->channel[c].aBank = val;
      break;
    }
    case 0x5: {
      dma->channel[c].size = (dma->channel[c].size & 0xff00) | val;
      break;
    }
    case 0x6: {
      dma->channel[c].size = (dma->channel[c].size & 0xff) | (val << 8);
      break;
    }
    case 0x7: {
      dma->channel[c].indBank = val;
      break;
    }
    case 0x8: {
      dma->channel[c].tableAdr = (dma->channel[c].tableAdr & 0xff00) | val;
      break;
    }
    case 0x9: {
      dma->channel[c].tableAdr = (dma->channel[c].tableAdr & 0xff) | (val << 8);
      break;
    }
    case 0xa: {
      dma->channel[c].repCount = val;
      break;
    }
    case 0xb:
    case 0xf: {
      dma->channel[c].unusedByte = val;
      break;
    }
    default: {
      break;
    }
  }
}

extern bool g_fail;

void dma_doDma(Dma* dma) {
  if(dma->dmaTimer > 0) {
    dma->dmaTimer -= 2;
    return;
  }
  // figure out first channel that is active
  int i = 0;
  for(i = 0; i < 8; i++) {
    if(dma->channel[i].dmaActive) {
      break;
    }
  }
  if(i == 8) {
    // no active channels
    dma->dmaBusy = false;
    return;
  }

  if (!dma->channel[i].fromB && (dma->channel[i].aBank & 0x80) && !(dma->channel[i].aAdr & 0x8000) && !g_fail) {
    printf("Warning! DMA from addr 0x%x\n", dma->channel[i].aBank << 16 | dma->channel[i].aAdr);
    g_fail = true;
  }

  // do channel i
  dma_transferByte(
    dma, dma->channel[i].aAdr, dma->channel[i].aBank,
    dma->channel[i].bAdr + bAdrOffsets[dma->channel[i].mode][dma->channel[i].offIndex++], dma->channel[i].fromB
  );
  dma->channel[i].offIndex &= 3;
  dma->dmaTimer += 6; // 8 cycles for each byte taken, -2 for this cycle
  if(!dma->channel[i].fixed) {
    dma->channel[i].aAdr += dma->channel[i].decrement ? -1 : 1;
  }
  dma->channel[i].size--;
  if(dma->channel[i].size == 0) {
    dma->channel[i].offIndex = 0; // reset offset index
    dma->channel[i].dmaActive = false;
    dma->dmaTimer += 8; // 8 cycle overhead per channel
  }
}

void dma_initHdma(Dma* dma) {
  dma->hdmaTimer = 0;
  bool hdmaHappened = false;
  for(int i = 0; i < 8; i++) {
    if(dma->channel[i].hdmaActive) {
      hdmaHappened = true;
      // terminate any dma
      dma->channel[i].dmaActive = false;
      dma->channel[i].offIndex = 0;
      // load address, repCount, and indirect address if needed
      dma->channel[i].tableAdr = dma->channel[i].aAdr;
      dma->channel[i].repCount = snes_read(dma->snes, (dma->channel[i].aBank << 16) | dma->channel[i].tableAdr++);
      if (dma->channel[i].repCount == 0) {
        dma->channel[i].terminated = true;
        continue;
      }
      dma->hdmaTimer += 8; // 8 cycle overhead for each active channel
      if(dma->channel[i].indirect) {
        dma->channel[i].size = snes_read(dma->snes, (dma->channel[i].aBank << 16) | dma->channel[i].tableAdr++);
        dma->channel[i].size |= snes_read(dma->snes, (dma->channel[i].aBank << 16) | dma->channel[i].tableAdr++) << 8;
        dma->hdmaTimer += 16; // another 16 cycles for indirect (total 24)
      }
      dma->channel[i].doTransfer = true;
    } else {
      dma->channel[i].doTransfer = false;
    }
    dma->channel[i].terminated = false;
  }
  if(hdmaHappened) dma->hdmaTimer += 16; // 18 cycles overhead, -2 for this cycle
}

/* Gate 5s/5t: A-bus -> VRAM fast path for one channel of dma_runAll(). Same end state as the byte loop:
 * VRAM words, dirty pages, VMADD, channel address / size / offset index. Handles mode 1 word pairs to $2118/$2119
 * (even length, VMAIN increments on the high byte) and, Gate 5t, mode 0 to $2118 (VMAIN increments after the low
 * byte) or to $2119 (after the high byte), as the Mode 7 map / tile uploads do. Taken only when the loop would not
 * branch differently: forward, not fixed, no address remap, the source is WRAM ($7E/$7F) or LoROM ($00-$5F, address
 * >= $8000) and does not wrap inside its bank (so the "DMA from addr" warning of the byte loop cannot fire).
 * Returns false (nothing touched) otherwise. Source bytes are read one at a time:
 * the source may be odd-aligned and Allegrex faults on unaligned halfword loads. */ * the source may be odd-aligned and Allegrex faults on unaligned halfword loads. */
static bool g_dma_rom_ok = true;
static unsigned g_dma_rom_checks;

static bool dma_fastVram(Dma* dma, DmaChannel* ch) {
  Snes* snes = dma->snes;
  Ppu* ppu = snes->ppu;
  uint32_t n = ch->size ? ch->size : 0x10000;
  if(ch->fromB || ch->fixed || ch->decrement || ch->offIndex != 0 || ppu->vramRemapMode != 0 ||
     (uint32_t)ch->aAdr + n > 0x10000)
    return false;
  /* kind 0: mode 1 word pairs; 1: mode 0 to $2118; 2: mode 0 to $2119. Any other combination (e.g. a byte written
   * to a register that does not advance the address, so every byte lands on one word) stays in the byte loop. */
  int kind;
  if(ch->mode == 1 && ch->bAdr == 0x18 && !(n & 1) && ppu->vramIncrementOnHigh)
    kind = 0;
  else if(ch->mode == 0 && ch->bAdr == 0x18 && !ppu->vramIncrementOnHigh)
    kind = 1;
  else if(ch->mode == 0 && ch->bAdr == 0x19 && ppu->vramIncrementOnHigh)
    kind = 2;
  else
    return false;
  uint8_t bank = ch->aBank;
  const uint8_t* src;
  if(bank == 0x7e || bank == 0x7f) {
    src = snes->ram + (((bank & 1) << 16) | ch->aAdr);
  } else if((bank & 0x7f) <= 0x5f && ch->aAdr >= 0x8000 && g_dma_rom_ok && g_rom != NULL) {
    src = g_rom + (((uint32_t)(bank & 0x7f) << 15) | (ch->aAdr & 0x7fff));
    if(g_dma_rom_checks < 32) {  /* the ROM mapping lives in cart.c: compare the first transfers with snes_read() */
      uint32_t a = (uint32_t)bank << 16 | ch->aAdr;
      g_dma_rom_checks++;
      if(src[0] != snes_read(snes, a) || src[n / 2] != snes_read(snes, a + n / 2) ||
         src[n - 1] != snes_read(snes, a + n - 1)) {
        g_dma_rom_ok = false;
        PspLog("dma: ROM fast path disabled (bank %02x addr %04x differs from cart_read)", bank, ch->aAdr);
        return false;
      }
    }
  } else {
    return false;
  }
  uint16_t* vram = ppu->vram;
  uint8_t* dirty = ppu->vramDirty;
  uint16_t ptr = ppu->vramPointer, inc = ppu->vramIncrement;
  if(kind == 0) {
    for(uint32_t k = 0; k < n; k += 2, src += 2, ptr += inc) {
      uint16_t a = ptr & 0x7fff;
      uint16_t nv = (uint16_t)(src[0] | src[1] << 8);
      if(nv != vram[a]) {  /* as ppu_write: an identical rewrite leaves the page clean */
        vram[a] = nv;
        dirty[a >> 8] = 1;
      }
    }
  } else {  /* one byte per word: low byte ($2118) or high byte ($2119), the other byte is kept */
    const unsigned keep = kind == 1 ? 0xff00u : 0x00ffu, sh = kind == 1 ? 0u : 8u;
    for(uint32_t k = 0; k < n; k++, src++, ptr += inc) {
      uint16_t a = ptr & 0x7fff;
      uint16_t old = vram[a];
      uint16_t nv = (uint16_t)((old & keep) | ((unsigned)src[0] << sh));
      if(nv != old) {
        vram[a] = nv;
        dirty[a >> 8] = 1;
      }
    }
  }
  ppu->vramPointer = ptr;
  ch->aAdr = (uint16_t)(ch->aAdr + n);
  ch->size = 0;
  ch->offIndex = 0;
  ch->dmaActive = false;
  return true;
}

/* Runs every channel enabled by dma_startDma(..., false) to completion, in the order, with the address, size and
 * offset-index updates of dma_doDma(). $420B DMA is synchronous (nothing else runs while the cycle timers count
 * down), so only their end state matters: the timers and dmaBusy end at 0 / false, as when dma_cycle() is looped. */
void dma_runAll(Dma* dma) {
  for(int i = 0; i < 8; i++) {
    DmaChannel* ch = &dma->channel[i];
    if(ch->dmaActive && dma_fastVram(dma, ch))
      continue;
    while(ch->dmaActive) {
      if(!g_fail && !ch->fromB && (ch->aBank & 0x80) && !(ch->aAdr & 0x8000)) {
        printf("Warning! DMA from addr 0x%x\n", ch->aBank << 16 | ch->aAdr);
        g_fail = true;
      }
      dma_transferByte(dma, ch->aAdr, ch->aBank, ch->bAdr + bAdrOffsets[ch->mode][ch->offIndex++], ch->fromB);
      ch->offIndex &= 3;
      if(!ch->fixed) {
        ch->aAdr += ch->decrement ? -1 : 1;
      }
      ch->size--;
      if(ch->size == 0) {
        ch->offIndex = 0;
        ch->dmaActive = false;
      }
    }
  }
  dma->hdmaTimer = 0;
  dma->dmaTimer = 0;
  dma->dmaBusy = false;
}

void dma_doHdma(Dma* dma) {
  dma->hdmaTimer = 0;
  bool hdmaHappened = false;
  for(int i = 0; i < 8; i++) {
    if(dma->channel[i].hdmaActive && !dma->channel[i].terminated) {
//      printf("DMA %d: 0x%x\n", i, dma->channel[i].bAdr);
      hdmaHappened = true;
      // terminate any dma
      dma->channel[i].dmaActive = false;
      dma->channel[i].offIndex = 0;
      // do the hdma
      dma->hdmaTimer += 8; // 8 cycles overhead for each active channel

      
      if(dma->channel[i].doTransfer) {
        for(int j = 0; j < transferLength[dma->channel[i].mode]; j++) {
          dma->hdmaTimer += 8; // 8 cycles for each byte transferred
          if(dma->channel[i].indirect) {
            dma_transferByte(
              dma, dma->channel[i].size++, dma->channel[i].indBank,
              dma->channel[i].bAdr + bAdrOffsets[dma->channel[i].mode][j], dma->channel[i].fromB
            );
          } else {
            dma_transferByte(
              dma, dma->channel[i].tableAdr++, dma->channel[i].aBank,
              dma->channel[i].bAdr + bAdrOffsets[dma->channel[i].mode][j], dma->channel[i].fromB
            );
          }
        }
      }
      dma->channel[i].repCount--;
      dma->channel[i].doTransfer = dma->channel[i].repCount & 0x80;
      if((dma->channel[i].repCount & 0x7f) == 0) {
        dma->channel[i].repCount = snes_read(dma->snes, (dma->channel[i].aBank << 16) | dma->channel[i].tableAdr++);
        if(dma->channel[i].indirect) {
          // TODO: oddness with not fetching high byte if last active channel and reCount is 0
          dma->channel[i].size = snes_read(dma->snes, (dma->channel[i].aBank << 16) | dma->channel[i].tableAdr++);
          dma->channel[i].size |= snes_read(dma->snes, (dma->channel[i].aBank << 16) | dma->channel[i].tableAdr++) << 8;
          dma->hdmaTimer += 16; // 16 cycles for new indirect address
        }
        if(dma->channel[i].repCount == 0) dma->channel[i].terminated = true;
        dma->channel[i].doTransfer = true;
      }
    }
  }
  if(hdmaHappened) dma->hdmaTimer += 16; // 18 cycles overhead, -2 for this cycle
}

static void dma_transferByte(Dma* dma, uint16_t aAdr, uint8_t aBank, uint8_t bAdr, bool fromB) {
  // TODO: invalid writes:
  //   accesing b-bus via a-bus gives open bus,
  //   $2180-$2183 while accessing ram via a-bus open busses $2180-$2183
  //   cannot access $4300-$437f (dma regs), or $420b / $420c
  if(fromB) {
    snes_write(dma->snes, (aBank << 16) | aAdr, snes_readBBus(dma->snes, bAdr));
  } else {
    snes_writeBBus(dma->snes, bAdr, snes_read(dma->snes, (aBank << 16) | aAdr));
  }
}

bool dma_cycle(Dma* dma) {
  if(dma->hdmaTimer > 0) {
    dma->hdmaTimer -= 2;
    return true;
  } else if(dma->dmaBusy) {
    dma_doDma(dma);
    return true;
  }
  return false;
}

void dma_startDma(Dma* dma, uint8_t val, bool hdma) {
  for(int i = 0; i < 8; i++) {
    if(hdma) {
      dma->channel[i].hdmaActive = val & (1 << i);
    } else {
      dma->channel[i].dmaActive = val & (1 << i);
    }
  }
  if(!hdma) {
    dma->dmaBusy = val;
    dma->dmaTimer += dma->dmaBusy ? 16 : 0; // 12-24 cycle overhead for entire dma transfer
  }
}
