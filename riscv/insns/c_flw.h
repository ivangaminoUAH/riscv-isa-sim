require_extension('C');
if (xlen == 32) {
  require_extension('F');
  require_fp;
  WRITE_RVC_FRS2S(f32(MMU.load<uint32_t>(RVC_RS1S + insn.rvc_lw_imm())));
} else { // c.ld
  WRITE_RVC_RS2S(MMU.load<int64_t>(RVC_RS1S + insn.rvc_ld_imm()));
}
