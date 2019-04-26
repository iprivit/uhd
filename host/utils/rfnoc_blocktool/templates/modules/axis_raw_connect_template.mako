<%page args="num_inputs, num_outputs"/>\
\
%for idx, input in enumerate(config.data.inputs):
  // Payload Stream to User Logic: ${input.name}
  .m_${input.name}_payload_tdata(${input.name}_payload_tdata),
  .m_${input.name}_payload_tkeep(${input.name}_payload_tkeep),
  .m_${input.name}_payload_tlast(${input.name}_payload_tlast),
  .m_${input.name}_payload_tvalid(${input.name}_payload_tvalid),
  .m_${input.name}_payload_tready(${input.name}_payload_tready),
  // Context Stream to User Logic: ${input.name}
  .m_${input.name}_context_tdata(${input.name}_context_tdata),
  .m_${input.name}_context_tuser(${input.name}_context_tuser),
  .m_${input.name}_context_tlast(${input.name}_context_tlast),
  .m_${input.name}_context_tvalid(${input.name}_context_tvalid),
  .m_${input.name}_context_tready(${input.name}_context_tready)${"," if (idx < num_inputs - 1) or (num_outputs > 0) else ""}
%endfor

%for idx, output in enumerate(config.data.outputs):
  // Payload Stream from User Logic: ${output.name}
  .s_${output.name}_payload_tdata(${output.name}_payload_tdata),
  .s_${output.name}_payload_tkeep(${output.name}_payload_tkeep),
  .s_${output.name}_payload_tlast(${output.name}_payload_tlast),
  .s_${output.name}_payload_tvalid(${output.name}_payload_tvalid),
  .s_${output.name}_payload_tready(${output.name}_payload_tready),
  // Context Stream from User Logic: ${output.name}
  .s_${output.name}_context_tdata(${output.name}_context_tdata),
  .s_${output.name}_context_tuser(${output.name}_context_tuser),
  .s_${output.name}_context_tlast(${output.name}_context_tlast),
  .s_${output.name}_context_tvalid(${output.name}_context_tvalid),
  .s_${output.name}_context_tready(${output.name}_context_tready)${"," if (idx < num_outputs -1) else ""}
%endfor