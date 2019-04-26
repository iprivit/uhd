<%page args="num_inputs, num_outputs"/>

%for idx, input in enumerate(config.data.inputs):
  .m_${input.name}_chdr_tdata(${input.name}_chdr_tdata),
  .m_${input.name}_chdr_tlast(${input.name}_chdr_tlast),
  .m_${input.name}_chdr_tvalid(${input.name}_chdr_tvalid),
  .m_${input.name}_chdr_tready(${input.name}_chdr_tready)${"," if (idx < num_inputs -1) or (num_outputs > 0) else ""}
%endfor

%for idx, output in enumerate(config.data.outputs):
  .s_${output.name}_chdr_tdata(${output.name}_chdr_tdata),
  .s_${output.name}_chdr_tlast(${output.name}_chdr_tlast),
  .s_${output.name}_chdr_tvalid(${output.name}_chdr_tvalid),
  .s_${output.name}_chdr_tready(${output.name}_chdr_tready)${"," if (idx < num_outputs -1) else ""}
%endfor
