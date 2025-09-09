
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
#include "pub_tool_tooliface.h"
#include "pub_tool_options.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_machine.h"
#include "pub_tool_mallocfree.h"

#include <limits.h>

/*------------------------------------------------------------*/
/*--- Command line options                                 ---*/
/*------------------------------------------------------------*/

static Bool clo_print_deads = False;

static Bool dw_process_cmd_line_option(const HChar* arg)
{
   if VG_BOOL_CLO(arg, "--print-deads", clo_print_deads) {}
   else
      return False;
   return True;
}

static void dw_print_usage(void)
{  
   VG_(printf)(
"    --print-deads=no|yes     print dead writes [no]\n"
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

static const IndexInfo W[] = {
   {20, 44}, {20, 24}, {20, 4}, {4, 0},
};

static Addr get_index(Addr addr, IndexInfo info)
{
   Addr w = info.width;
   Addr lsb = info.lsb;
   return (addr >> lsb) & ((1 << w) - 1);
}

#define LEVELS (sizeof(W) / sizeof(W[0]))

union PageDir {
   union PageDir* subtables;
   UShort*        shadow_mem;
};

static union PageDir pagetable;

static ULong n_writes = 0;
static ULong n_dead_writes = 0;

static inline UShort *get_word(Addr addr)
{
   union PageDir* pde = &pagetable;

   #pragma GCC unroll 100
   for (SizeT i = 0; i < LEVELS - 1; ++i) {
      Addr idx = get_index(addr, W[i]);

      if (i < LEVELS - 2) {
         if (UNLIKELY(!pde->subtables))
            pde->subtables = VG_(calloc)("get_word", 1 << W[i].width, sizeof(union PageDir));
         pde = &pde->subtables[idx];
         continue;
      }

      if (UNLIKELY(!pde->shadow_mem))
         pde->shadow_mem = VG_(calloc)("get_word", 1 << W[i].width, sizeof(UShort));

      return &pde->shadow_mem[idx];
   }

   tl_assert(0);
}

static VG_REGPARM(2) void dw_load(Addr addr, SizeT size)
{
//   VG_(printf)(" L %08lx,%lu\n", addr, size);

   for (SizeT i = 0; i < size; ++i) {
      Addr    byte_addr = addr + i;
      // TODO: we can reuse word from previous iteration
      UShort* w = get_word(byte_addr);
      Addr    offset = get_index(byte_addr, W[LEVELS - 1]);
      *w &= ~(UShort)(1 << offset);
   }
}

static inline void report_dead_write(Addr base, SizeT start, SizeT end) {
   if (!clo_print_deads)
      return;
   // TODO: report call stack
   VG_(printf)("Dead write of %lu byte(s) at %08lx\n", end - start, base + start);
}

static VG_REGPARM(2) void dw_store(Addr addr, SizeT size)
{
//   VG_(printf)(" S %08lx,%lu\n", addr, size);

   Int first_dead_write = -1;

   for (SizeT i = 0; i < size; ++i) {
      Addr    byte_addr = addr + i;
      // TODO: we can reuse word from previous iteration
      UShort* w = get_word(byte_addr);
      Addr    offset = get_index(byte_addr, W[LEVELS - 1]);
      ++n_writes;
      if (*w & (1 << offset)) {
         if (first_dead_write < 0)
            first_dead_write = i;
         ++n_dead_writes;
      } else if (first_dead_write >= 0) {
         report_dead_write(addr, first_dead_write, i);
      }
      *w |= 1 << offset;
   }

   if (first_dead_write >= 0)
      report_dead_write(addr, first_dead_write, size);
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
   }
   Event;

#define N_EVENTS 4

static Event events[N_EVENTS];
static Int   events_used = 0;

static void flushEvents(IRSB* sb) {
   Int        i;
   const HChar* helperName;
   void*      helperAddr;
   IRExpr**   argv;
   IRDirty*   di;

   for (i = 0; i < events_used; i++) {
      Event* ev = &events[i];

      if (ev->read) {
         helperAddr = dw_load;
         helperName = "dw_load";
      } else {
         helperAddr = dw_store;
         helperName = "dw_store";
      }

      argv = mkIRExprVec_2( ev->addr, mkIRExpr_HWord( ev->size ) );
      di   = unsafeIRDirty_0_N( /*regparms*/2, 
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
void addEvent_guarded ( IRSB* sb, IRExpr* addr, Int size, IRExpr* guard, Bool read )
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
   ++events_used;
}

/* Add an ordinary read event, by adding a guarded read event with an
   always-true guard. */
static
void addEvent ( IRSB* sb, IRExpr* addr, Int size, Bool read )
{
   addEvent_guarded(sb, addr, size, NULL, read);
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
            addEvent( sbOut, mkIRExpr_HWord( (HWord)st->Ist.IMark.addr ),
                      st->Ist.IMark.len, True );
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_WrTmp: {
            IRExpr* data = st->Ist.WrTmp.data;
            if (data->tag == Iex_Load) {
               addEvent( sbOut, data->Iex.Load.addr,
                         sizeofIRType(data->Iex.Load.ty), True );
            }
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_AbiHint:
            addEvent( sbOut, st->Ist.AbiHint.base,
                      st->Ist.AbiHint.len, True );
            addStmtToIRSB( sbOut, st );
            break;

         case Ist_Store: {
            IRExpr* data = st->Ist.Store.data;
            IRType  type = typeOfIRExpr(tyenv, data);
            tl_assert(type != Ity_INVALID);
            addEvent( sbOut, st->Ist.Store.addr,
                      sizeofIRType(type), False );
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_StoreG: {
            IRStoreG* sg   = st->Ist.StoreG.details;
            IRExpr*   data = sg->data;
            IRType    type = typeOfIRExpr(tyenv, data);
            tl_assert(type != Ity_INVALID);
            addEvent_guarded( sbOut, sg->addr,
                              sizeofIRType(type), sg->guard, False );
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
                              sizeofIRType(type), lg->guard, True );
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
                  addEvent( sbOut, d->mAddr, dsize, True );
               if (d->mFx == Ifx_Write || d->mFx == Ifx_Modify)
                  addEvent( sbOut, d->mAddr, dsize, False );
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
            addEvent( sbOut, cas->addr, dataSize, True );
            addEvent( sbOut, cas->addr, dataSize, False );
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_LLSC: {
            IRType dataTy;
            if (st->Ist.LLSC.storedata == NULL) {
               /* LL */
               dataTy = typeOfIRTemp(tyenv, st->Ist.LLSC.result);
               addEvent( sbOut, st->Ist.LLSC.addr,
                         sizeofIRType(dataTy), True );
               /* flush events before LL, helps SC to succeed */
               flushEvents(sbOut);
            } else {
               /* SC */
               dataTy = typeOfIRExpr(tyenv, st->Ist.LLSC.storedata);
               addEvent( sbOut, st->Ist.LLSC.addr,
                         sizeofIRType(dataTy), False );
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

static void dw_fini(Int exitcode) {
   VG_(umsg)("Detected %u%% dead writes (%llu out of %llu total)\n",
             (unsigned)(100.0 * n_dead_writes / n_writes), n_dead_writes,
             n_writes);
}

static void dw_pre_clo_init(void)
{
   /* Sanity checks */
   Addr w = 0;
   for (SizeT i = 0; i < LEVELS - 1; ++i) {
      tl_assert(W[i].lsb == W[i + 1].lsb + W[i + 1].width);
      w += W[i].width;
   }
   tl_assert(w + W[LEVELS - 1].width == sizeof(Addr) * CHAR_BIT);
   tl_assert((1 << W[LEVELS - 1].width) == sizeof(*pagetable.shadow_mem) * CHAR_BIT);

   VG_(details_name)            ("DeadWrites");
   VG_(details_version)         (NULL);
   VG_(details_description)     ("Valgrind tool to detect dead writes");
   VG_(details_copyright_author)(
      "Copyright (C) 2002-2024, and GNU GPL'd, by Nicholas Nethercote et al.");
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
