// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// ppc.h: the Model 3's main CPU, a PowerPC 603e (603e or 603ev/603r
// depending on the board step), emulated as an interpreter.
//
// The functions below are the whole interface the rest of the emulator uses.
// The CPU fetches instructions straight from host memory through a list of
// fetch regions (each word stored in host byte order) and does every data
// access through the IBus it is attached to.

#ifndef INCLUDED_PPC_H
#define INCLUDED_PPC_H

#include "BlockFile.h"
#include "Types.h"

// Special purpose register numbers that callers outside the core use (the
// block address translation pairs and the page table base, printed by the
// SDL front end's state dump).
#define SPR603E_SDR1    25
#define SPR603E_IBAT0U  528
#define SPR603E_IBAT0L  529
#define SPR603E_IBAT1U  530
#define SPR603E_IBAT1L  531
#define SPR603E_IBAT2U  532
#define SPR603E_IBAT2L  533
#define SPR603E_IBAT3U  534
#define SPR603E_IBAT3L  535
#define SPR603E_DBAT0U  536
#define SPR603E_DBAT0L  537
#define SPR603E_DBAT1U  538
#define SPR603E_DBAT1L  539
#define SPR603E_DBAT2U  540
#define SPR603E_DBAT2L  541
#define SPR603E_DBAT3U  542
#define SPR603E_DBAT3L  543

// Processor version register values (version in the upper half, revision
// in the lower half).
typedef enum {
  PPC_MODEL_603   = 0x00030000,
  PPC_MODEL_603E  = 0x00060103,   // 603e, revision 1.3
  PPC_MODEL_603EV = 0x00070000,
  PPC_MODEL_603R  = 0x00071202    // 603r (603ev family), revision 2.1
} PPC_MODEL;

// Bus clock the CPU's PLL multiplies (the index into the PLL tables).
typedef enum {
  BUS_FREQUENCY_16MHZ = 0,
  BUS_FREQUENCY_20MHZ,
  BUS_FREQUENCY_25MHZ,
  BUS_FREQUENCY_33MHZ,
  BUS_FREQUENCY_40MHZ,
  BUS_FREQUENCY_50MHZ,
  BUS_FREQUENCY_60MHZ,
  BUS_FREQUENCY_66MHZ,
  BUS_FREQUENCY_75MHZ
} PPC_BUS_FREQUENCY;

// CPU configuration. bus_frequency_multiplier is the core:bus clock ratio
// written as two BCD-like digits: 0x15 is 1.5, 0x25 is 2.5, 0x10 is 1.0.
typedef struct {
  PPC_MODEL         pvr;
  int               bus_frequency_multiplier;
  PPC_BUS_FREQUENCY bus_frequency;
} PPC_CONFIG;

// A block of guest address space instructions can be fetched from directly.
// end is inclusive. A list of regions ends with an entry whose ptr is NULL.
typedef struct {
  UINT32  start;
  UINT32  end;
  UINT32  *ptr;
} PPC_FETCH_REGION;

// Setup (in this order: init, attach a bus, give the fetch regions, reset).
extern void   ppc_init(const PPC_CONFIG *config);
extern void   ppc_attach_bus(class IBus *BusPtr);
extern void   ppc_set_fetch(PPC_FETCH_REGION *fetch);
extern void   ppc_reset(void);
extern void   ppc_shutdown(void);

// Running and interrupts.
extern int    ppc_execute(int cycles);    // returns the cycles actually run
extern void   ppc_set_irq_line(int irqline);
extern UINT64 ppc_total_cycles(void);
extern int    ppc_get_cycles_per_sec(void);
extern int    ppc_get_bus_freq_multipler(void);
extern int    ppc_get_timer_ratio(void);
extern void   ppc_set_timer_ratio(int ratio);

// Save states (block "PowerPC").
extern void   ppc_save_state(class CBlockFile *SaveState);
extern void   ppc_load_state(class CBlockFile *SaveState);

// Register access.
extern UINT32 ppc_get_pc(void);
extern void   ppc_set_pc(UINT32 pc);
extern UINT32 ppc_get_gpr(unsigned num);
extern void   ppc_set_gpr(unsigned num, UINT32 val);
extern double ppc_get_fpr(unsigned num);
extern void   ppc_set_fpr(unsigned num, double val);
extern UINT8  ppc_get_cr(unsigned num);
extern void   ppc_set_cr(unsigned num, UINT8 val);
extern UINT32 ppc_get_lr(void);
extern UINT32 ppc_read_spr(unsigned spr);
extern void   ppc_write_spr(unsigned spr, UINT32 val);
extern UINT32 ppc_read_sr(unsigned num);
extern void   ppc_write_sr(unsigned num, UINT32 val);
extern UINT32 ppc_read_msr(void);
extern void   ppc_break(void);

#endif  // INCLUDED_PPC_H
