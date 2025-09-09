
/*--------------------------------------------------------------------*/
/*--- DeadWrites: Valgrind tool to detect dead writes.   dw_main.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of DeadWrites, a Valgrind tool,
   which detects dead writes.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.

   The GNU General Public License is contained in the file COPYING.
*/

#include "pub_tool_basics.h"
#include "pub_tool_execontext.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_machine.h"
#include "pub_tool_mallocfree.h"
#include "pub_tool_options.h"
#include "pub_tool_threadstate.h"
#include "pub_tool_tooliface.h"

#include <limits.h>

/*------------------------------------------------------------*/
/*--- Command line options                                 ---*/
/*------------------------------------------------------------*/

#define DEFAULT_SAMPLE_RATE 100
#define DEFAULT_NUM_STACKS 20

static Bool clo_detailed_counts = False;
static Bool clo_fast_instrument = False;
static Int clo_num_stacks       = DEFAULT_NUM_STACKS;
static Bool clo_track_code      = False;
// TODO: also randomized sampling ?
static Int clo_sample_rate      = DEFAULT_SAMPLE_RATE;

static Bool dw_process_cmd_line_option(const HChar* arg)
{
   if VG_BOOL_CLO(arg, "--detailed-counts", clo_detailed_counts) {}
   else if VG_BOOL_CLO(arg, "--fast-instrument", clo_fast_instrument) {}
   else if VG_INT_CLO(arg, "--num-stacks", clo_num_stacks) {}
   else if VG_INT_CLO(arg, "--track-code", clo_track_code) {}
   else if VG_INT_CLO(arg, "--sample-rate", clo_sample_rate) {}
   else
      return False;
   return True;
}

#define S(X) #X

static void dw_print_usage(void)
{
   VG_(printf)(
"    --detailed-counts=no|yes     print stats for each callstack [no]\n"
"    --fast-instrument=no|yes     instrument each memory access immediately (debug flag, will reduce performance) [no]\n"
"    --num-stacks=N               print only top N callstacks [" S(DEFAULT_NUM_STACKS) "]\n"
"    --track-code=no|yes          detect writes to code segments (will reduce performance) [no]\n"
"    --sample-rate=N              callstack collection rate [" S(DEFAULT_SAMPLE_RATE) "]\n"
   );
}

static void dw_print_debug_usage(void)
{
   VG_(printf)(
"    (none)\n"
   );
}

/*------------------------------------------------------------*/
/*--- Callbacks                                            ---*/
/*------------------------------------------------------------*/

typedef
   struct {
      Addr width;
      Addr lsb;
   }
   IndexInfo;

// TODO: make last table larger for performance ?
static const IndexInfo W[] = {
   {20, 44}, {20, 24}, {20, 4}, {4, 0},
};

static inline SizeT get_index(Addr addr, IndexInfo info)
{
   Addr w   = info.width;
   Addr lsb = info.lsb;
   return (addr >> lsb) & ((1 << w) - 1);
}

#define LEVELS (sizeof(W) / sizeof(W[0]))

union PageDir {
   union PageDir* subtables;
   UInt*          shadow_mem;
};

static union PageDir pagetable;

#define ECU_INC 4

typedef
   struct {
      UInt  ecu;
      SizeT count;
   }
   StatEntry;

static StatEntry *stats;
static SizeT num_ecu       = 0;
static Int stack_sample    = 0;
static ULong n_writes      = 0;
static ULong n_dead_writes = 0;

// TODO: we can detect at 8-byte granularity (but will have false negatives)
// TODO: we can reuse shadow_mem found in previous iteration
static inline UInt* get_info(Addr addr)
{
   union PageDir* pde = &pagetable;

   #pragma GCC unroll 100
   for (SizeT i = 0; i < LEVELS; ++i) {
      SizeT idx = get_index(addr, W[i]);

      if (i < LEVELS - 1) {
         if (UNLIKELY(!pde->subtables))
            pde->subtables = VG_(calloc)("get_info", 1 << W[i].width, sizeof(union PageDir));
         pde = &pde->subtables[idx];
         continue;
      }

      if (UNLIKELY(!pde->shadow_mem))
         pde->shadow_mem = VG_(calloc)("get_info", 1 << W[i].width, sizeof(*pde->shadow_mem));

      return &pde->shadow_mem[idx];
   }

   tl_assert(0);
}

static VG_REGPARM(2) void dw_load(Addr addr, SizeT size, SizeT ip)
{
   for (SizeT i = 0; i < size; ++i) {
      Addr  byte_addr = addr + i;
      UInt* ecu       = get_info(byte_addr);
      *ecu = 0;
   }
}

static VG_REGPARM(2) void dw_store(Addr addr, SizeT size, SizeT ip)
{
   UInt     new_ecu;
   ThreadId tid = VG_(get_running_tid)();

#if defined(VGA_x86) || defined(VGA_amd64)
   // Hack from deadstores tool (see guest_amd64_toIR.c):
   // ignore false positives in 0(%RSP) writes
   if (VG_(get_SP(tid)) == addr)
      return;
#endif

   if (LIKELY(!clo_detailed_counts || stack_sample++ % clo_sample_rate)) {
      new_ecu = 0;
   } else {
      Addr cur_IP         = VG_(get_IP)( tid );
      Addr first_ip_delta = ip ? ip - cur_IP : 0;
      ExeContext* here    = VG_(record_ExeContext)( tid, first_ip_delta );

      new_ecu = VG_(get_ECU_from_ExeContext)(here);
      if (UNLIKELY(new_ecu / ECU_INC >= num_ecu)) {
         UInt new_num_ecu = num_ecu ? 2 * num_ecu : 4096;
         if (new_num_ecu <= new_ecu / ECU_INC)
            new_num_ecu = new_ecu / ECU_INC + 1;
         stats = VG_(realloc)("dw_store", stats, new_num_ecu * sizeof(StatEntry));
         for (SizeT i = num_ecu; i < new_num_ecu; ++i) {
            stats[i].ecu   = i * ECU_INC;
            stats[i].count = 0;
         }
         num_ecu = new_num_ecu;
      }
   }

   n_writes += size;

   for (SizeT i = 0; i < size; ++i) {
      Addr  byte_addr = addr + i;
      UInt* ecu       = get_info(byte_addr);
      if (*ecu & 1) {
         UInt real_ecu = *ecu & ~(UInt)1;
         if (clo_detailed_counts && real_ecu) {
            if (real_ecu == 5808)
               VG_(printf)("XXX\n");
            ++stats[real_ecu / ECU_INC].count;
         }
         ++n_dead_writes;
      }
      *ecu = new_ecu | 1;
   }
}

/*------------------------------------------------------------*/
/*--- Instrumentation                                      ---*/
/*------------------------------------------------------------*/

typedef
   struct {
      IRExpr* addr;
      Bool    read;
      Int     size;
      IRExpr* guard; /* :: Ity_I1, or NULL=="always True" */
      Addr    ip;
   }
   Event;

#define N_EVENTS 4

static Event events[N_EVENTS];
static Int   events_used = 0;

static void flushEvents(IRSB* sb)
{
   Int      i;
   const HChar* helperName;
   void*    helperAddr;
   IRExpr** argv;
   IRDirty* di;

   for (i = 0; i < events_used; i++) {
      Event* ev = &events[i];

      if (ev->read) {
         helperAddr = dw_load;
         helperName = "dw_load";
      } else {
         helperAddr = dw_store;
         helperName = "dw_store";
      }

      argv = mkIRExprVec_3( ev->addr, mkIRExpr_HWord( ev->size ),
                            mkIRExpr_HWord( ev->ip) );
      di   = unsafeIRDirty_0_N( /*regparms*/3,
                                helperName, VG_(fnptr_to_fnentry)( helperAddr ),
                                argv );
      if (ev->guard) {
         di->guard = ev->guard;
      }
      addStmtToIRSB( sb, IRStmt_Dirty(di) );
   }

   events_used = 0;
}

static
void addEvent_guarded ( IRSB* sb, IRExpr* addr, Int size, IRExpr* guard,
                        Addr ip, Bool read )
{
   Event* evt;

   if (events_used == N_EVENTS)
      flushEvents(sb);
   tl_assert(events_used >= 0 && events_used < N_EVENTS);

   evt        = &events[events_used];
   evt->read  = read;
   evt->addr  = addr;
   evt->size  = size;
   evt->guard = guard;
   evt->ip    = ip;

   ++events_used;

   if (clo_fast_instrument) {
      evt->ip = 0;
      flushEvents(sb);
   }
}

/* Add an ordinary read event, by adding a guarded read event with an
   always-true guard. */
static
void addEvent ( IRSB* sb, IRExpr* addr, Int size, Addr ip, Bool read )
{
   addEvent_guarded(sb, addr, size, NULL, ip, read);
}

static void dw_post_clo_init(void)
{
   /* Unless we are actually tracking file descriptors we act as if we don't
      handle any errors.  */
   if (!VG_(clo_track_fds))
     VG_(needs_core_errors)(False);
}

static
IRSB* dw_instrument ( VgCallbackClosure* closure,
                      IRSB* sbIn,
                      const VexGuestLayout* layout,
                      const VexGuestExtents* vge,
                      const VexArchInfo* archinfo_host,
                      IRType gWordTy, IRType hWordTy )
{
   Int        i;
   IRSB*      sbOut;
   IRTypeEnv* tyenv = sbIn->tyenv;
   Addr       ip = 0;

   /* Set up SB */
   sbOut = deepCopyIRSBExceptStmts(sbIn);

   // Copy verbatim any IR preamble preceding the first IMark
   i = 0;
   while (i < sbIn->stmts_used && sbIn->stmts[i]->tag != Ist_IMark) {
      addStmtToIRSB( sbOut, sbIn->stmts[i] );
      i++;
   }

   for (/*use current i*/; i < sbIn->stmts_used; i++) {
      IRStmt* st = sbIn->stmts[i];
      if (!st || st->tag == Ist_NoOp) continue;

      switch (st->tag) {
         case Ist_NoOp:
         case Ist_Put:
         case Ist_PutI:
         case Ist_MBE:
            addStmtToIRSB( sbOut, st );
            break;

         case Ist_IMark: {
            ip = st->Ist.IMark.addr + st->Ist.IMark.delta;
            if (clo_track_code) {
                addEvent( sbOut, mkIRExpr_HWord( (HWord)st->Ist.IMark.addr ),
                          st->Ist.IMark.len, ip, True );
            }
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_WrTmp: {
            IRExpr* data = st->Ist.WrTmp.data;
            if (data->tag == Iex_Load) {
               addEvent( sbOut, data->Iex.Load.addr,
                         sizeofIRType(data->Iex.Load.ty), ip, True );
            }
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_AbiHint:
            addEvent( sbOut, st->Ist.AbiHint.base,
                      st->Ist.AbiHint.len, ip, True );
            addStmtToIRSB( sbOut, st );
            break;

         case Ist_Store: {
            IRExpr* data = st->Ist.Store.data;
            IRType  type = typeOfIRExpr(tyenv, data);
            tl_assert(type != Ity_INVALID);
            addEvent( sbOut, st->Ist.Store.addr,
                      sizeofIRType(type), ip, False );
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_StoreG: {
            IRStoreG* sg   = st->Ist.StoreG.details;
            IRExpr*   data = sg->data;
            IRType    type = typeOfIRExpr(tyenv, data);
            tl_assert(type != Ity_INVALID);
            addEvent_guarded( sbOut, sg->addr,
                              sizeofIRType(type), sg->guard, ip, False );
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_LoadG: {
            IRLoadG* lg       = st->Ist.LoadG.details;
            IRType   type     = Ity_INVALID; /* loaded type */
            IRType   typeWide = Ity_INVALID; /* after implicit widening */
            typeOfIRLoadGOp(lg->cvt, &typeWide, &type);
            tl_assert(type != Ity_INVALID);
            addEvent_guarded( sbOut, lg->addr,
                              sizeofIRType(type), lg->guard, ip, True );
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_Dirty: {
            Int      dsize;
            IRDirty* d = st->Ist.Dirty.details;
            if (d->mFx != Ifx_None) {
               // This dirty helper accesses memory.  Collect the details.
               tl_assert(d->mAddr != NULL);
               tl_assert(d->mSize != 0);
               dsize = d->mSize;
               if (d->mFx == Ifx_Read || d->mFx == Ifx_Modify)
                  addEvent( sbOut, d->mAddr, dsize, ip, True );
               if (d->mFx == Ifx_Write || d->mFx == Ifx_Modify)
                  addEvent( sbOut, d->mAddr, dsize, ip, False );
            } else {
               tl_assert(d->mAddr == NULL);
               tl_assert(d->mSize == 0);
            }
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_CAS: {
            /* We treat it as a read and a write of the location. */
            Int    dataSize;
            IRType dataTy;
            IRCAS* cas = st->Ist.CAS.details;
            tl_assert(cas->addr != NULL);
            tl_assert(cas->dataLo != NULL);
            dataTy   = typeOfIRExpr(tyenv, cas->dataLo);
            dataSize = sizeofIRType(dataTy);
            if (cas->dataHi != NULL)
               dataSize *= 2; /* since it's a doubleword-CAS */
            addEvent( sbOut, cas->addr, dataSize, ip, True );
            addEvent( sbOut, cas->addr, dataSize, ip, False );
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_LLSC: {
            IRType dataTy;
            if (st->Ist.LLSC.storedata == NULL) {
               /* LL */
               dataTy = typeOfIRTemp(tyenv, st->Ist.LLSC.result);
               addEvent( sbOut, st->Ist.LLSC.addr,
                         sizeofIRType(dataTy), ip, True );
               /* flush events before LL, helps SC to succeed */
               flushEvents(sbOut);
            } else {
               /* SC */
               dataTy = typeOfIRExpr(tyenv, st->Ist.LLSC.storedata);
               addEvent( sbOut, st->Ist.LLSC.addr,
                         sizeofIRType(dataTy), ip, False );
            }
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_Exit:
            flushEvents(sbOut);
            addStmtToIRSB( sbOut, st );
            break;

         default:
            ppIRStmt(st);
            tl_assert(0);
      }
   }

   /* At the end of the sbIn.  Flush outstandings. */
   flushEvents(sbOut);

   return sbOut;
}

static int print_stats_cmp(const void* a, const void* b)
{
   const StatEntry* ea = a;
   const StatEntry* eb = b;
   return ea->count > eb->count ? -1 : ea->count < eb->count ? 1 : 0;
}

static void print_stats(void)
{
   VG_(ssort)(stats, num_ecu, sizeof(*stats), print_stats_cmp);

   VG_(umsg)("Collected %lu\n", num_ecu);
   for (SizeT i = 0; i < VG_MIN(num_ecu, (SizeT)clo_num_stacks); ++i) {
      StatEntry entry = stats[i];
      if (entry.count && entry.ecu) {
         ExeContext* ec = VG_(get_ExeContext_from_ECU)(entry.ecu);
         VG_(pp_ExeContext)(ec);
         VG_(umsg)("%lu\n", entry.count);
      }
   }
}

static void dw_fini(Int exitcode)
{
   VG_(umsg)("Detected %u%% dead writes (%llu out of %llu total)\n",
             (unsigned)(100.0 * n_dead_writes / n_writes), n_dead_writes,
             n_writes);
   if (clo_detailed_counts)
      print_stats();
}

static void dw_pre_clo_init(void)
{
   /* Sanity checks */
   SizeT prev_lsb = sizeof(Addr) * CHAR_BIT;
   for (SizeT i = 0; i < LEVELS; ++i) {
      tl_assert(prev_lsb == W[i].lsb + W[i].width);
      prev_lsb = W[i].lsb;
   }
   tl_assert(prev_lsb == 0);

   VG_(details_name)            ("DeadWrites");
   VG_(details_version)         (NULL);
   VG_(details_description)     ("Valgrind tool to detect dead writes");
   VG_(details_copyright_author)(
      "Copyright (C) 2002-2024, and GNU GPL'd, by Yuri Gribov et al.");
   VG_(details_bug_reports_to)  (VG_BUGS_TO);

   VG_(details_avg_translation_sizeB) ( 275 );

   VG_(basic_tool_funcs)        (dw_post_clo_init,
                                 dw_instrument,
                                 dw_fini);
   VG_(needs_command_line_options)(dw_process_cmd_line_option,
                                   dw_print_usage,
                                   dw_print_debug_usage);
   VG_(needs_xml_output)        ();
   VG_(needs_core_errors)       (True); /* Yes, but... see dw_post_clo_init  */

   /* No needs, no core events to track */
}

VG_DETERMINE_INTERFACE_VERSION(dw_pre_clo_init)

/*--------------------------------------------------------------------*/
/*--- end                                                          ---*/
/*--------------------------------------------------------------------*/
