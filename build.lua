local spirv = require("@lito.spirv")
local target = lito.target({ kind = "test", name = "vrento" })
local inputs = {}
local content = {
  "export module vrento.tests.shaders;",
  "export namespace vrento_test::shaders {",
  "template <decltype(sizeof(0)) N> struct Code { unsigned words[N]; };",
  "template <decltype(sizeof(0)) N> consteval auto decode(const unsigned char (&bytes)[N]) {",
  "  static_assert(N >= 20 && N % 4 == 0);",
  "  Code<N / 4> result{};",
  "  for (decltype(N) i = 0; i < N / 4; ++i)",
  "    result.words[i] = unsigned(bytes[i*4]) | (unsigned(bytes[i*4+1]) << 8) |",
  "      (unsigned(bytes[i*4+2]) << 16) | (unsigned(bytes[i*4+3]) << 24);",
  "  return result;",
  "}",
}
for _, shader in ipairs({
  { name = "vertex", source = "cache.vert", stage = "vertex" },
  { name = "fragment", source = "cache.frag", stage = "fragment" },
  { name = "graph_fragment", source = "graph.frag", stage = "fragment" },
  { name = "buffer_fragment", source = "buffer.frag", stage = "fragment" },
  { name = "buffer_compute", source = "buffer.comp", stage = "compute" },
}) do
  local result = spirv.compile({
    backend = "glslang", tool = lito.tool("glslang"), language = "glsl",
    source = "tests/shaders/" .. shader.source, stage = shader.stage,
    target_env = "vulkan1.1", output = "tests/" .. shader.source .. ".spv",
  })
  inputs[#inputs + 1] = result.spirv
  content[#content + 1] = "inline constexpr auto " .. shader.name .. " = decode({"
  content[#content + 1] = '#embed "@INPUT:' .. #inputs .. '@"'
  content[#content + 1] = "});"
end
content[#content + 1] = "}"
local source = lito.write({
  output = "tests/shaders.cppm", inputs = inputs,
  content = table.concat(content, "\n") .. "\n",
})
lito.target_add_generated_source(target, source.output)
