// ARM32 is a bit special from the linker's viewpoint because ARM
// processors support two different instruction encodings: Thumb and
// ARM (in a narrower sense). Thumb instructions are either 16 bits or
// 32 bits, while ARM instructions are all 32 bits. Feature-wise,
// Thumb is a subset of ARM, so not all ARM instructions are
// representable in Thumb.
//
// ARM processors originally supported only ARM instructions. Thumb
// instructions were later added to increase code density.
//
// ARM processors runs in either ARM mode or Thumb mode. The mode can
// be switched using BX (branch and mode exchange)-family instructions.
// We need to use that instructions to, for example, call a function
// encoded in Thumb from a function encoded in ARM. Sometimes, the
// linker even has to emit interworking thunk code to switch mode.
//
// ARM instructions are aligned to 4 byte boundaries. Thumb are to 2
// byte boundaries. So the least significant bit of a function address
// is always 0.
//
// To distinguish Thumb functions from ARM fucntions, the LSB of a
// function address is repurposed as a boolean flag. If the LSB is 0,
// the function referred to by the address is encoded in ARM;
// otherwise, Thumb.
//
// For example, if a symbol `foo` is of type STT_FUNC and has value
// 0x2001, `foo` is a function using Thumb instructions whose address
// is 0x2000 (not 0x2001, as Thumb instructions are always 2-byte
// aligned). Likewise, if a function pointer has value 0x2001, it
// refers a Thumb function at 0x2000.
//
// https://github.com/ARM-software/abi-aa/blob/main/aaelf32/aaelf32.rst

#include "mold.h"

#include <tbb/parallel_for.h>

namespace mold {

using E = ARM32;

template <>
i64 get_addend(u8 *loc, const ElfRel<E> &rel) {
  ul32 *arm = (ul32 *)loc;
  ul16 *thm = (ul16 *)loc;

  switch (rel.r_type) {
  case R_ARM_ABS32:
  case R_ARM_REL32:
  case R_ARM_BASE_PREL:
  case R_ARM_GOTOFF32:
  case R_ARM_GOT_PREL:
  case R_ARM_GOT_BREL:
  case R_ARM_TLS_GD32:
  case R_ARM_TLS_LDM32:
  case R_ARM_TLS_LDO32:
  case R_ARM_TLS_IE32:
  case R_ARM_TLS_LE32:
  case R_ARM_TLS_GOTDESC:
  case R_ARM_TARGET1:
  case R_ARM_TARGET2:
    return (il32)*arm;
  case R_ARM_THM_JUMP8:
    return sign_extend(thm[0], 8) << 1;
  case R_ARM_THM_JUMP11:
    return sign_extend(thm[0], 11) << 1;
  case R_ARM_THM_JUMP19: {
    // https://developer.arm.com/documentation/ddi0597/2024-12/Base-Instructions/B--Branch-
    u32 S = bit(thm[0], 10);
    u32 J2 = bit(thm[1], 11);
    u32 J1 = bit(thm[1], 13);
    u32 imm6 = bits(thm[0], 5, 0);
    u32 imm11 = bits(thm[1], 10, 0);
    u32 val = (S << 20) | (J2 << 19) | (J1 << 18) | (imm6 << 12) | (imm11 << 1);
    return sign_extend(val, 21);
  }
  case R_ARM_THM_CALL:
  case R_ARM_THM_JUMP24:
  case R_ARM_THM_TLS_CALL: {
    // https://developer.arm.com/documentation/ddi0597/2024-12/Base-Instructions/BL--BLX--immediate---Branch-with-Link-and-optional-Exchange--immediate--
    u32 S = bit(thm[0], 10);
    u32 J1 = bit(thm[1], 13);
    u32 J2 = bit(thm[1], 11);
    u32 I1 = !(J1 ^ S);
    u32 I2 = !(J2 ^ S);
    u32 imm10 = bits(thm[0], 9, 0);
    u32 imm11 = bits(thm[1], 10, 0);
    u32 val = (S << 24) | (I1 << 23) | (I2 << 22) | (imm10 << 12) | (imm11 << 1);
    return sign_extend(val, 25);
  }
  case R_ARM_CALL:
  case R_ARM_JUMP24:
  case R_ARM_PLT32:
  case R_ARM_TLS_CALL:
    return sign_extend(*arm, 24) << 2;
  case R_ARM_MOVW_PREL_NC:
  case R_ARM_MOVW_ABS_NC:
  case R_ARM_MOVT_PREL:
  case R_ARM_MOVT_ABS: {
    // https://developer.arm.com/documentation/ddi0597/2024-12/Base-Instructions/MOV--MOVS--immediate---Move--immediate--
    u32 imm4 = bits(*arm, 19, 16);
    u32 imm12 = bits(*arm, 11, 0);
    u32 val = (imm4 << 12) | imm12;
    return sign_extend(val, 16);
  }
  case R_ARM_PREL31:
    return sign_extend(*arm, 31);
  case R_ARM_THM_MOVW_PREL_NC:
  case R_ARM_THM_MOVW_ABS_NC:
  case R_ARM_THM_MOVT_PREL:
  case R_ARM_THM_MOVT_ABS: {
    // https://developer.arm.com/documentation/ddi0597/2024-12/Base-Instructions/MOVT--Move-Top-
    u32 imm4 = bits(thm[0], 3, 0);
    u32 i = bit(thm[0], 10);
    u32 imm3 = bits(thm[1], 14, 12);
    u32 imm8 = bits(thm[1], 7, 0);
    u32 val = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8;
    return sign_extend(val, 16);
  }
  default:
    return 0;
  }
}

static void write_arm_mov(u8 *loc, u32 val) {
  u32 imm12 = bits(val, 11, 0);
  u32 imm4 = bits(val, 15, 12);
  *(ul32 *)loc = (*(ul32 *)loc & 0xfff0'f000) | (imm4 << 16) | imm12;
}

static void write_thm_b21(u8 *loc, u32 val) {
  u32 S = bit(val, 20);
  u32 J2 = bit(val, 19);
  u32 J1 = bit(val, 18);
  u32 imm6 = bits(val, 17, 12);
  u32 imm11 = bits(val, 11, 1);

  ul16 *buf = (ul16 *)loc;
  buf[0] = (buf[0] & 0b1111'1011'1100'0000) | (S << 10) | imm6;
  buf[1] = (buf[1] & 0b1101'0000'0000'0000) | (J1 << 13) | (J2 << 11) | imm11;
}

static void write_thm_b25(u8 *loc, u32 val) {
  u32 S = bit(val, 24);
  u32 I1 = bit(val, 23);
  u32 I2 = bit(val, 22);
  u32 J1 = !I1 ^ S;
  u32 J2 = !I2 ^ S;
  u32 imm10 = bits(val, 21, 12);
  u32 imm11 = bits(val, 11, 1);

  ul16 *buf = (ul16 *)loc;
  buf[0] = (buf[0] & 0b1111'1000'0000'0000) | (S << 10) | imm10;
  buf[1] = (buf[1] & 0b1101'0000'0000'0000) | (J1 << 13) | (J2 << 11) | imm11;
}

static void write_thm_mov(u8 *loc, u32 val) {
  u32 imm4 = bits(val, 15, 12);
  u32 i = bit(val, 11);
  u32 imm3 = bits(val, 10, 8);
  u32 imm8 = bits(val, 7, 0);

  ul16 *buf = (ul16 *)loc;
  buf[0] = (buf[0] & 0b1111'1011'1111'0000) | (i << 10) | imm4;
  buf[1] = (buf[1] & 0b1000'1111'0000'0000) | (imm3 << 12) | imm8;
}

template <>
void write_addend(u8 *loc, i64 val, const ElfRel<E> &rel) {
  switch (rel.r_type) {
  case R_ARM_NONE:
    break;
  case R_ARM_ABS32:
  case R_ARM_REL32:
  case R_ARM_BASE_PREL:
  case R_ARM_GOTOFF32:
  case R_ARM_GOT_PREL:
  case R_ARM_GOT_BREL:
  case R_ARM_TLS_GD32:
  case R_ARM_TLS_LDM32:
  case R_ARM_TLS_LDO32:
  case R_ARM_TLS_IE32:
  case R_ARM_TLS_LE32:
  case R_ARM_TLS_GOTDESC:
  case R_ARM_TARGET1:
  case R_ARM_TARGET2:
    *(ul32 *)loc = val;
    break;
  case R_ARM_THM_JUMP8:
    *(ul16 *)loc = (*(ul16 *)loc & 0xff00) | bits(val, 8, 1);
    break;
  case R_ARM_THM_JUMP11:
    *(ul16 *)loc = (*(ul16 *)loc & 0xf800) | bits(val, 11, 1);
    break;
  case R_ARM_THM_CALL:
  case R_ARM_THM_JUMP24:
  case R_ARM_THM_TLS_CALL:
    write_thm_b25(loc, val);
    break;
  case R_ARM_CALL:
  case R_ARM_JUMP24:
  case R_ARM_PLT32:
    *(ul32 *)loc = (*(ul32 *)loc & 0xff00'0000) | bits(val, 25, 2);
    break;
  case R_ARM_MOVW_PREL_NC:
  case R_ARM_MOVW_ABS_NC:
  case R_ARM_MOVT_PREL:
  case R_ARM_MOVT_ABS:
    write_arm_mov(loc, val);
    break;
  case R_ARM_PREL31:
    *(ul32 *)loc = (*(ul32 *)loc & 0x8000'0000) | (val & 0x7fff'ffff);
    break;
  case R_ARM_THM_MOVW_PREL_NC:
  case R_ARM_THM_MOVW_ABS_NC:
  case R_ARM_THM_MOVT_PREL:
  case R_ARM_THM_MOVT_ABS:
    write_thm_mov(loc, val);
    break;
  default:
    unreachable();
  }
}

template <>
void write_plt_header(Context<E> &ctx, u8 *buf) {
  constexpr ul32 insn[] = {
    0xe52d'e004, //    push {lr}
    0xe59f'e004, //    ldr lr, 2f
    0xe08f'e00e, // 1: add lr, pc, lr
    0xe5be'f008, //    ldr pc, [lr, #8]!
    0x0000'0000, // 2: .word .got.plt - 1b - 8
    0x0000'0000, //    (padding)
    0x0000'0000, //    (padding)
    0x0000'0000, //    (padding)
  };

  memcpy(buf, insn, sizeof(insn));
  *(ul32 *)(buf + 16) = ctx.gotplt->shdr.sh_addr - ctx.plt->shdr.sh_addr - 16;
}

static constexpr ul32 plt_entry[] = {
  0xe59f'c004, // 1: ldr ip, 2f
  0xe08c'c00f, //    add ip, ip, pc
  0xe59c'f000, //    ldr pc, [ip]
  0x0000'0000, // 2: .word sym@GOT - 1b
};

template <>
void write_plt_entry(Context<E> &ctx, u8 *buf, Symbol<E> &sym) {
  memcpy(buf, plt_entry, sizeof(plt_entry));
  *(ul32 *)(buf + 12) = sym.get_gotplt_addr(ctx) - sym.get_plt_addr(ctx) - 12;
}

template <>
void write_pltgot_entry(Context<E> &ctx, u8 *buf, Symbol<E> &sym) {
  memcpy(buf, plt_entry, sizeof(plt_entry));
  *(ul32 *)(buf + 12) = sym.get_got_pltgot_addr(ctx) - sym.get_plt_addr(ctx) - 12;
}

template <>
void EhFrameSection<E>::apply_eh_reloc(Context<E> &ctx, const ElfRel<E> &rel,
                                       u64 offset, u64 val) {
  u8 *loc = ctx.buf + this->shdr.sh_offset + offset;

  switch (rel.r_type) {
  case R_NONE:
    break;
  case R_ARM_ABS32:
    *(ul32 *)loc = val;
    break;
  case R_ARM_REL32:
    *(ul32 *)loc = val - this->shdr.sh_addr - offset;
    break;
  default:
    Fatal(ctx) << "unsupported relocation in .eh_frame: " << rel;
  }
}

static Thunk<E> &get_reachable_thunk(OutputSection<E> &osec, u64 addr) {
  auto it = std::upper_bound(osec.thunks.begin(), osec.thunks.end(), addr,
                             [](u64 addr, std::unique_ptr<Thunk<E>> &thunk) {
    return addr < thunk->get_addr();
  });
  return **it;
}

template <>
void InputSection<E>::apply_reloc_alloc(Context<E> &ctx, u8 *base) {
  std::span<const ElfRel<E>> rels = get_rels(ctx);
  RelocationsStats rels_stats;

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_NONE || rel.r_type == R_ARM_V4BX)
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];
    u8 *loc = base + rel.r_offset;

    u64 S = sym.get_addr(ctx);
    u64 A = get_addend(*this, rel);
    u64 P = get_addr() + rel.r_offset;
    u64 T = S & 1;
    u64 G = sym.get_got_idx(ctx) * sizeof(Word<E>);
    u64 GOT = ctx.got->shdr.sh_addr;

    auto check = [&](i64 val, i64 lo, i64 hi) {
      if (ctx.arg.stats)
        update_relocation_stats(rels_stats, i, val, lo, hi);
      check_range(ctx, i, val, lo, hi);
    };

    auto get_thumb_thunk_addr = [&] { return sym.get_thunk_addr(ctx, P); };
    auto get_arm_thunk_addr   = [&] { return sym.get_thunk_addr(ctx, P) + 4; };

    auto get_tlsdesc_trampoline_addr = [&] {
      return get_reachable_thunk(*output_section, P).get_addr();
    };

    switch (rel.r_type) {
    case R_ARM_ABS32:
    case R_ARM_TARGET1:
      break;
    case R_ARM_REL32:
      *(ul32 *)loc = S + A - P;
      break;
    case R_ARM_THM_CALL: {
      if (sym.is_remaining_undef_weak()) {
        // On ARM, calling an weak undefined symbol jumps to the
        // next instruction.
        *(ul32 *)loc = 0x8000'f3af; // NOP.W
        break;
      }

      // THM_CALL relocation refers to either BL or BLX instruction.
      // They are different in only one bit. We need to use BL if
      // the jump target is Thumb. Otherwise, use BLX.
      i64 val1 = S + A - P;
      i64 val2 = align_to(S + A - P, 4);

      if (T && is_int(val1, 25)) {
        *(ul16 *)(loc + 2) |= 0x1000;  // BL
        write_thm_b25(loc, val1);
      } else if (!T && is_int(val2, 25)) {
        *(ul16 *)(loc + 2) &= ~0x1000; // BLX
        write_thm_b25(loc, val2);
      } else {
        *(ul16 *)(loc + 2) |= 0x1000;  // BL
        write_thm_b25(loc, get_thumb_thunk_addr() + A - P);
      }
      break;
    }
    case R_ARM_BASE_PREL:
      *(ul32 *)loc = GOT + A - P;
      break;
    case R_ARM_GOTOFF32:
      *(ul32 *)loc = ((S + A) | T) - GOT;
      break;
    case R_ARM_GOT_PREL:
    case R_ARM_TARGET2:
      *(ul32 *)loc = GOT + G + A - P;
      break;
    case R_ARM_GOT_BREL:
      *(ul32 *)loc = G + A;
      break;
    case R_ARM_CALL: {
      if (sym.is_remaining_undef_weak()) {
        *(ul32 *)loc = 0xe320'f000; // NOP
        break;
      }

      // Just like THM_CALL, ARM_CALL relocation refers to either BL or
      // BLX instruction. We may need to rewrite BL → BLX or BLX → BL.
      bool is_bl = ((*(ul32 *)loc & 0xff00'0000) == 0xeb00'0000);
      bool is_blx = ((*(ul32 *)loc & 0xfe00'0000) == 0xfa00'0000);
      if (!is_bl && !is_blx)
        Fatal(ctx) << *this << ": R_ARM_CALL refers to neither BL nor BLX";

      i64 val = S + A - P;
      if (is_int(val, 26)) {
        if (T) {
          *(ul32 *)loc = 0xfa00'0000; // BLX
          *(ul32 *)loc |= (bit(val, 1) << 24) | bits(val, 25, 2);
        } else {
          *(ul32 *)loc = 0xeb00'0000; // BL
          *(ul32 *)loc |= bits(val, 25, 2);
        }
      } else {
        *(ul32 *)loc = 0xeb00'0000; // BL
        *(ul32 *)loc |= bits(get_arm_thunk_addr() + A - P, 25, 2);
      }
      break;
    }
    case R_ARM_JUMP24: {
      if (sym.is_remaining_undef_weak()) {
        *(ul32 *)loc = 0xe320'f000; // NOP
        break;
      }

      // These relocs refers to a B (unconditional branch) instruction.
      // Unlike BL or BLX, we can't rewrite B to BX in place when the
      // processor mode switch is required because BX doesn't takes an
      // immediate; it takes only a register. So if mode switch is
      // required, we jump to a linker-synthesized thunk which does the
      // job with a longer code sequence.
      i64 val = S + A - P;
      if (T || !is_int(val, 26))
        val = get_arm_thunk_addr() + A - P;
      *(ul32 *)loc = (*(ul32 *)loc & 0xff00'0000) | bits(val, 25, 2);
      break;
    }
    case R_ARM_PLT32:
      if (sym.is_remaining_undef_weak()) {
        *(ul32 *)loc = 0xe320'f000; // NOP
      } else {
        u64 val = (T ? get_arm_thunk_addr() : S) + A - P;
        *(ul32 *)loc = (*(ul32 *)loc & 0xff00'0000) | bits(val, 25, 2);
      }
      break;
    case R_ARM_THM_JUMP8:
      check(S + A - P, -(1 << 8), 1 << 8);
      *(ul16 *)loc &= 0xff00;
      *(ul16 *)loc |= bits(S + A - P, 8, 1);
      break;
    case R_ARM_THM_JUMP11:
      check(S + A - P, -(1 << 11), 1 << 11);
      *(ul16 *)loc &= 0xf800;
      *(ul16 *)loc |= bits(S + A - P, 11, 1);
      break;
    case R_ARM_THM_JUMP19:
      check(S + A - P, -(1 << 20), 1 << 20);
      write_thm_b21(loc, S + A - P);
      break;
    case R_ARM_THM_JUMP24: {
      if (sym.is_remaining_undef_weak()) {
        *(ul32 *)loc = 0x8000'f3af; // NOP
        break;
      }

      // Just like R_ARM_JUMP24, we need to jump to a thunk if we need to
      // switch processor mode.
      i64 val = S + A - P;
      if (!T || !is_int(val, 25))
        val = get_thumb_thunk_addr() + A - P;
      write_thm_b25(loc, val);
      break;
    }
    case R_ARM_MOVW_PREL_NC:
      write_arm_mov(loc, ((S + A) | T) - P);
      break;
    case R_ARM_MOVW_ABS_NC:
      write_arm_mov(loc, (S + A) | T);
      break;
    case R_ARM_THM_MOVW_PREL_NC:
      write_thm_mov(loc, ((S + A) | T) - P);
      break;
    case R_ARM_PREL31:
      check(S + A - P, -(1LL << 30), 1LL << 30);
      *(ul32 *)loc &= 0x8000'0000;
      *(ul32 *)loc |= (S + A - P) & 0x7fff'ffff;
      break;
    case R_ARM_THM_MOVW_ABS_NC:
      write_thm_mov(loc, (S + A) | T);
      break;
    case R_ARM_MOVT_PREL:
      write_arm_mov(loc, (S + A - P) >> 16);
      break;
    case R_ARM_THM_MOVT_PREL:
      write_thm_mov(loc, (S + A - P) >> 16);
      break;
    case R_ARM_MOVT_ABS:
      write_arm_mov(loc, (S + A) >> 16);
      break;
    case R_ARM_THM_MOVT_ABS:
      write_thm_mov(loc, (S + A) >> 16);
      break;
    case R_ARM_TLS_GD32:
      *(ul32 *)loc = sym.get_tlsgd_addr(ctx) + A - P;
      break;
    case R_ARM_TLS_LDM32:
      *(ul32 *)loc = ctx.got->get_tlsld_addr(ctx) + A - P;
      break;
    case R_ARM_TLS_LDO32:
      *(ul32 *)loc = S + A - ctx.dtp_addr;
      break;
    case R_ARM_TLS_IE32:
      *(ul32 *)loc = sym.get_gottp_addr(ctx) + A - P;
      break;
    case R_ARM_TLS_LE32:
      *(ul32 *)loc = S + A - ctx.tp_addr;
      break;
    case R_ARM_TLS_GOTDESC:
      // ARM32 TLSDESC uses the following code sequence to materialize
      // a TP-relative address in r0.
      //
      //       ldr     r0, .L2
      //  .L1: bl      foo
      //           R_ARM_TLS_CALL
      //  .L2: .word   foo + . - .L1
      //           R_ARM_TLS_GOTDESC
      //
      // We may relax the instructions to the following if its TP-relative
      // address is known at link-time
      //
      //       ldr     r0, .L2
      //  .L1: nop
      //       ...
      //  .L2: .word   foo(tpoff)
      //
      // or to the following if the TP-relative address is known at
      // process startup time.
      //
      //       ldr     r0, .L2
      //  .L1: ldr r0, [pc, r0]
      //       ...
      //  .L2: .word   foo(gottpoff) + . - .L1
      if (sym.has_tlsdesc(ctx)) {
        // A is odd if the corresponding TLS_CALL is Thumb.
        *(ul32 *)loc = sym.get_tlsdesc_addr(ctx) - P + A - ((A & 1) ? 6 : 4);
      } else if (sym.has_gottp(ctx)) {
        *(ul32 *)loc = sym.get_gottp_addr(ctx) - P + A - ((A & 1) ? 5 : 8);
      } else {
        *(ul32 *)loc = S - ctx.tp_addr;
      }
      break;
    case R_ARM_TLS_CALL:
      if (sym.has_tlsdesc(ctx)) {
        *(ul32 *)loc = 0xeb00'0000; // bl 0
        *(ul32 *)loc |= bits(get_tlsdesc_trampoline_addr() - P - 8, 25, 2);
      } else if (sym.has_gottp(ctx)) {
        *(ul32 *)loc = 0xe79f'0000; // ldr r0, [pc, r0]
      } else {
        *(ul32 *)loc = 0xe320'f000; // nop
      }
      break;
    case R_ARM_THM_TLS_CALL:
      if (sym.has_tlsdesc(ctx)) {
        u64 val = align_to(get_tlsdesc_trampoline_addr() - P - 4, 4);
        write_thm_b25(loc, val);
        *(ul16 *)(loc + 2) &= ~0x1000; // rewrite BL with BLX
      } else if (sym.has_gottp(ctx)) {
        // Since `ldr r0, [pc, r0]` is not representable in Thumb,
        // we use two instructions instead.
        *(ul16 *)loc = 0x4478;         // add r0, pc
        *(ul16 *)(loc + 2) = 0x6800;   // ldr r0, [r0]
      } else {
        *(ul32 *)loc = 0x8000'f3af;    // nop.w
      }
      break;
    default:
      Error(ctx) << *this << ": unknown relocation: " << rel;
    }
  }
  if (ctx.arg.stats)
    save_relocation_stats<E>(ctx, *this, rels_stats);
}

template <>
void InputSection<E>::apply_reloc_nonalloc(Context<E> &ctx, u8 *base) {
  std::span<const ElfRel<E>> rels = get_rels(ctx);

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_NONE || record_undef_error(ctx, rel))
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];
    u8 *loc = base + rel.r_offset;

    SectionFragment<E> *frag;
    i64 frag_addend;
    std::tie(frag, frag_addend) = get_fragment(ctx, rel);

    u64 S = frag ? frag->get_addr(ctx) : sym.get_addr(ctx);
    u64 A = frag ? frag_addend : get_addend(*this, rel);

    switch (rel.r_type) {
    case R_ARM_ABS32:
      if (std::optional<u64> val = get_tombstone(sym, frag))
        *(ul32 *)loc = *val;
      else
        *(ul32 *)loc = S + A;
      break;
    case R_ARM_TLS_LDO32:
      if (std::optional<u64> val = get_tombstone(sym, frag))
        *(ul32 *)loc = *val;
      else
        *(ul32 *)loc = S + A - ctx.dtp_addr;
      break;
    default:
      Fatal(ctx) << *this << ": invalid relocation for non-allocated sections: "
                 << rel;
      break;
    }
  }
}

template <>
void InputSection<E>::scan_relocations(Context<E> &ctx) {
  assert(shdr().sh_flags & SHF_ALLOC);
  std::span<const ElfRel<E>> rels = get_rels(ctx);

  // Scan relocations
  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_NONE || record_undef_error(ctx, rel))
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];

    if (sym.is_ifunc())
      sym.flags |= NEEDS_GOT | NEEDS_PLT;

    switch (rel.r_type) {
    case R_ARM_MOVW_ABS_NC:
    case R_ARM_THM_MOVW_ABS_NC:
      scan_absrel(ctx, sym, rel);
      break;
    case R_ARM_THM_CALL:
    case R_ARM_CALL:
    case R_ARM_JUMP24:
    case R_ARM_PLT32:
    case R_ARM_THM_JUMP24:
      if (sym.is_imported)
        sym.flags |= NEEDS_PLT;
      break;
    case R_ARM_GOT_PREL:
    case R_ARM_GOT_BREL:
    case R_ARM_TARGET2:
      sym.flags |= NEEDS_GOT;
      break;
    case R_ARM_MOVT_PREL:
    case R_ARM_THM_MOVT_PREL:
    case R_ARM_PREL31:
      scan_pcrel(ctx, sym, rel);
      break;
    case R_ARM_TLS_GD32:
      sym.flags |= NEEDS_TLSGD;
      break;
    case R_ARM_TLS_LDM32:
      ctx.needs_tlsld = true;
      break;
    case R_ARM_TLS_IE32:
      sym.flags |= NEEDS_GOTTP;
      break;
    case R_ARM_TLS_CALL:
    case R_ARM_THM_TLS_CALL:
      scan_tlsdesc(ctx, sym);
      break;
    case R_ARM_TLS_LE32:
      check_tlsle(ctx, sym, rel);
      break;
    case R_ARM_ABS32:
    case R_ARM_TARGET1:
    case R_ARM_MOVT_ABS:
    case R_ARM_THM_MOVT_ABS:
    case R_ARM_REL32:
    case R_ARM_BASE_PREL:
    case R_ARM_GOTOFF32:
    case R_ARM_THM_JUMP8:
    case R_ARM_THM_JUMP11:
    case R_ARM_THM_JUMP19:
    case R_ARM_MOVW_PREL_NC:
    case R_ARM_THM_MOVW_PREL_NC:
    case R_ARM_TLS_LDO32:
    case R_ARM_V4BX:
    case R_ARM_TLS_GOTDESC:
      break;
    default:
      Error(ctx) << *this << ": unknown relocation: " << rel;
    }
  }
}

template <>
void Thunk<E>::copy_buf(Context<E> &ctx) {
  // TLS trampoline code. ARM32's TLSDESC is designed so that this
  // common piece of code is factored out from object files to reduce
  // output size. Since no one provide, the linker has to synthesize it.
  constexpr ul32 hdr[] = {
    0xe08e'0000, // add r0, lr, r0
    0xe590'1004, // ldr r1, [r0, #4]
    0xe12f'ff11, // bx  r1
    0xe320'f000, // nop
  };

  // This is a range extension and mode switch thunk.
  // It has two entry points: +0 for Thumb and +4 for ARM.
  constexpr u8 entry[] = {
    // .thumb
    0x78, 0x47,             //    bx   pc  # jumps to 1f
    0xc0, 0x46,             //    nop
    // .arm
    0x00, 0xc0, 0x9f, 0xe5, // 1: ldr  ip, 3f
    0x0f, 0xf0, 0x8c, 0xe0, // 2: add  pc, ip, pc
    0x00, 0x00, 0x00, 0x00, // 3: .word sym - 2b
  };

  static_assert(E::thunk_hdr_size == sizeof(hdr));
  static_assert(E::thunk_size == sizeof(entry));

  u8 *buf = ctx.buf + output_section.shdr.sh_offset + offset;
  memcpy(buf, hdr, sizeof(hdr));
  buf += sizeof(hdr);

  u64 P = output_section.shdr.sh_addr + offset + sizeof(hdr);

  for (Symbol<E> *sym : symbols) {
    memcpy(buf, entry, sizeof(entry));
    *(ul32 *)(buf + 12) = sym->get_addr(ctx) - P - 16;

    buf += sizeof(entry);
    P += sizeof(entry);
  }
}

template <>
u64 get_eflags(Context<E> &ctx) {
  return EF_ARM_EABI_VER5;
}

void create_arm_exidx_section(Context<E> &ctx) {
  for (i64 i = 0; i < ctx.chunks.size(); i++) {
    OutputSection<E> *osec = ctx.chunks[i]->to_osec();

    if (osec && osec->shdr.sh_type == SHT_ARM_EXIDX) {
      auto *sec = new Arm32ExidxSection(*osec);
      ctx.extra.exidx = sec;
      ctx.chunks[i] = sec;
      ctx.chunk_pool.emplace_back(sec);

      for (InputSection<E> *isec : osec->members)
        isec->is_alive = false;
      break;
    }
  }
}

void Arm32ExidxSection::compute_section_size(Context<E> &ctx) {
  output_section.compute_section_size(ctx);
  this->shdr.sh_size = output_section.shdr.sh_size + 8; // +8 for sentinel
}

void Arm32ExidxSection::update_shdr(Context<E> &ctx) {
  // .ARM.exidx's sh_link should be set to the .text section index.
  // Runtime doesn't care about it, but the binutils's strip command does.
  if (Chunk<E> *chunk = find_chunk(ctx, ".text"))
    this->shdr.sh_link = chunk->shndx;
}

void Arm32ExidxSection::remove_duplicate_entries(Context<E> &ctx) {
  this->shdr.sh_size = get_contents(ctx).size();
}

void Arm32ExidxSection::copy_buf(Context<E> &ctx) {
  std::vector<u8> contents = get_contents(ctx);
  assert(this->shdr.sh_size == contents.size());
  write_vector(ctx.buf + this->shdr.sh_offset, contents);
}

// Returns the end of the text segment
static u64 get_text_end(Context<E> &ctx) {
  u64 ret = 0;
  for (Chunk<E> *chunk : ctx.chunks)
    if (chunk->shdr.sh_flags & SHF_EXECINSTR)
      ret = std::max<u64>(ret, chunk->shdr.sh_addr + chunk->shdr.sh_size);
  return ret;
}

// ARM executables use an .ARM.exidx section to look up an exception
// handling record for the current instruction pointer. The table needs
// to be sorted by their addresses.
//
// Other target uses .eh_frame_hdr instead for the same purpose.
// I don't know why only ARM uses the different mechanism, but it's
// likely that it's due to some historical reason.
//
// This function returns contents of .ARM.exidx.
std::vector<u8> Arm32ExidxSection::get_contents(Context<E> &ctx) {
  // .ARM.exidx records consists of a signed 31-bit relative address
  // and a 32-bit value. The relative address indicates the start
  // address of a function that the record covers. The value is one of
  // the followings:
  //
  // 1. CANTUNWIND indicating that there's no unwinding info for the function,
  // 2. a compact unwinding record encoded into a 32-bit value, or
  // 3. a 31-bit relative address which points to a larger record in
  //    the .ARM.extab section.
  //
  // CANTUNWIND is value 1. The most significant bit is set in (2) but
  // not in (3). So we can distinguished them just by looking at a value.
  const u32 CANTUNWIND = 1;

  struct Entry {
    ul32 addr;
    ul32 val;
  };

  // We reserve one extra slot for the sentinel
  i64 num_entries = output_section.shdr.sh_size / sizeof(Entry) + 1;
  std::vector<u8> buf(num_entries * sizeof(Entry));
  Entry *ent = (Entry *)buf.data();

  // Write section contents to the buffer
  output_section.shdr.sh_addr = this->shdr.sh_addr;
  output_section.write_to(ctx, buf.data());

  // Fill in sentinel fields
  u64 sentinel_addr = this->shdr.sh_addr + sizeof(Entry) * (num_entries - 1);
  ent[num_entries - 1].addr = get_text_end(ctx) - sentinel_addr;
  ent[num_entries - 1].val = CANTUNWIND;

  // Entry's addresses are relative to themselves. In order to sort
  // records by address, we first translate them so that the addresses
  // are relative to the beginning of the section.
  auto is_relative = [](u32 val) {
    return val != CANTUNWIND && !(val & 0x8000'0000);
  };

  tbb::parallel_for((i64)0, num_entries, [&](i64 i) {
    i64 offset = sizeof(Entry) * i;
    ent[i].addr = sign_extend(ent[i].addr, 31) + offset;
    if (is_relative(ent[i].val))
      ent[i].val = 0x7fff'ffff & (ent[i].val + offset);
  });

  std::sort(ent, ent + num_entries, [](const Entry &a, const Entry &b) {
    return a.addr < b.addr;
  });

  // Remove duplicate adjacent entries. That is, if two adjacent functions
  // have the same compact unwind info or are both CANTUNWIND, we can
  // merge them into a single address range.
  auto it = std::unique(ent, ent + num_entries,
                        [](const Entry &a, const Entry &b) {
    return a.val == b.val;
  });

  num_entries = it - ent;
  buf.resize(num_entries * sizeof(Entry));

  // Make addresses relative to themselves.
  tbb::parallel_for((i64)0, num_entries, [&](i64 i) {
    i64 offset = sizeof(Entry) * i;
    ent[i].addr = 0x7fff'ffff & (ent[i].addr - offset);
    if (is_relative(ent[i].val))
      ent[i].val = 0x7fff'ffff & (ent[i].val - offset);
  });

  return buf;
}

} // namespace mold
