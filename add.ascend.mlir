module {
  func.func @add(%arg0: tensor<1024xf32>, %arg1: tensor<1024xf32>) -> tensor<1024xf32> attributes {hacc.entry, hacc.function_kind = #hacc.function_kind<DEVICE>} {
    %empty0 = tensor.empty() : tensor<1024xf32>
    %result0 = linalg.elemwise_binary {fun = #linalg.binary_fn<add>} ins(%arg0, %arg1 : tensor<1024xf32>, tensor<1024xf32>) outs(%empty0 : tensor<1024xf32>) -> tensor<1024xf32>
    return %result0 : tensor<1024xf32>
  }
}
