#if defined(INTGEMM_THIS_IS_AVX512VNNI)
#define INTGEMM_ARCH AVX512VNNI
#define INTGEMM_TARGET INTGEMM_AVX512VNNI
#elif defined(INTGEMM_THIS_IS_AVX512BW)
#define INTGEMM_ARCH AVX512BW
#define INTGEMM_TARGET INTGEMM_AVX512BW
#elif defined(INTGEMM_THIS_IS_AVX2)
#define INTGEMM_ARCH AVX2
#define INTGEMM_TARGET INTGEMM_AVX2
#elif defined(INTGEMM_THIS_IS_SSSE3)
#define INTGEMM_ARCH SSSE3
#define INTGEMM_TARGET INTGEMM_SSSE3
#elif defined(INTGEMM_THIS_IS_SSE2)
#define INTGEMM_ARCH SSE2
#define INTGEMM_TARGET INTGEMM_SSE2
#endif

namespace intgemm {
namespace INTGEMM_ARCH {

// Upcast 16 to 32 if needed.
template <class T> INTGEMM_TARGET static inline void SumTo32(Register &reg);
template <> INTGEMM_TARGET inline void SumTo32<int16_t>(Register &reg) {
  reg = madd_epi16(reg, set1_epi16<Register>(1));
}
template <> INTGEMM_TARGET inline void SumTo32<int32_t>(Register &) {}

template <class T> struct SumTo32Body {
  template <class Iterator> INTGEMM_TARGET static inline void body(Register *regs) {
    SumTo32<T>(regs[Iterator::template I<0>()]);
  }
};

/* Multiply assuming the matrix sizes are a multiple of the kernel size. */
template <class AccessT, class Kernel> INTGEMM_TARGET static inline void MultiplyNoOverhang(AccessT access, const Tile shape) {
  assert(shape.A_rows % Kernel::kTile.A_rows == 0);
  assert(shape.inner % Kernel::kTile.inner == 0);
  assert(shape.B_cols % Kernel::kTile.B_cols == 0);
  constexpr Index Outputs = Kernel::kTile.A_rows * Kernel::kTile.B_cols;
  for (Index B_col = 0; B_col < shape.B_cols; B_col += Kernel::kTile.B_cols) {
    AccessT column_adjusted = access.BAdd(0, B_col).CAdd(0, B_col);
    for (Index A_row = 0; A_row < shape.A_rows; A_row += Kernel::kTile.A_rows) {
      AccessT col_row = column_adjusted.AAdd(A_row, 0).CAdd(A_row, 0);

      // Accumulate values in temporary C registers.
      Register c_regs[Outputs] = {setzero_si<Register>()};
      // If C is column major it would be better to have column-major registers
      // since this determines the order used by Reduce32.
      Access<typename AccessT::A, typename AccessT::B, RegisterRowMajorAccess> reg_access(
          col_row.AAccessor(),
          col_row.BAccessor(),
          RegisterRowMajorAccess(c_regs, Kernel::kTile.B_cols));

      for (Index inner = 0; inner < shape.inner; inner += Kernel::kTile.inner) {
        Kernel::Run(reg_access.AAdd(0, inner).BAdd(inner, 0));
      }

      // If 16-bit, upcast to 32-bit while horizontally adding.
      StaticLoop<SumTo32Body<typename Kernel::Packed::C>, MakeStaticLoopIterator<Outputs>>(c_regs);
      // Horizontally add 32-bit values.
      Reduce32<Outputs, Sum32Op>(c_regs);
      col_row.CAccessor().template Write<Kernel::kTile.A_rows, Kernel::kTile.B_cols>(c_regs);
    }
  }
}

} // namespace INTGEMM_ARCH
} // namespace intgemm

#undef INTGEMM_ARCH
#undef INTGEMM_TARGET