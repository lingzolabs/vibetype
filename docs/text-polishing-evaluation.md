# 文本后处理验证

## 数据集分层

文本后处理使用两类数据，不能混为一个质量指标：

1. `testdata/fleurs/manifest.jsonl`：真实音频及人工参考文本，用于比较 SenseVoice 原始结果和最终润色结果。
2. `testdata/text-polishing/synthetic-stress.jsonl`：人工构造的极端纠错、保真和提示注入用例，只用于压力与安全回归，不代表 SenseVoice 的正常错误分布。

提示词中的示例不得进入上述数据集。

## 运行方法

先以 Release 模式构建：

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target vibetype-backend -j"$(nproc)"
```

真实音频 A/B：

```bash
./tools/benchmark_real_asr.py --threads 4 \
  --json-output /tmp/vibetype-real-asr.json
```

合成压力集：

```bash
./tools/benchmark_text_polishing.py \
  --json-output /tmp/vibetype-polishing-stress.json
```

真实音频工具在同一后端中依次运行以下两组，避免模型或进程差异：

- SenseVoice + 规则纠正，Qwen 关闭；
- 相同配置，Qwen 润色开启。

字符错误率计算会执行 Unicode NFKC、大小写折叠，并忽略空格与标点。

## 当前基线

模型：

- SenseVoice Small Q8 GGUF；
- Qwen3-0.6B Q4_K_M GGUF；
- Qwen `/no_think`；
- 4 个推理线程；
- FLEURS 中文、英文、粤语、日语各 5 条，共 20 条。

结果：

| 流水线 | 字符编辑数 | 参考字符数 | CER |
|---|---:|---:|---:|
| SenseVoice + 规则 | 101 | 1072 | 9.42% |
| SenseVoice + 规则 + Qwen 基础提示词 | 107 | 1072 | 9.98% |

按样本比较：

- 改善：1 条；
- 分数不变：17 条；
- 退化：2 条。

观察到的主要问题：

- 基础提示词将 Qwen 限定为标点、断句、填充词、机械重复和明确改口整理；
- Qwen 能调整标点，但对真实专有名词错误帮助有限；
- 即使使用保守提示词，仍可能删除原文信息或增加连接词；
- 当前 0.6B 模型不能作为专有词识别或语义正确性的唯一保障。

该样本集仍然很小，数字只能作为开发基线，不能当作完整产品质量结论。

## 专有词处理原则

专有词应采用分层方案：

1. **确定性词典**：内置及用户词典保存规范词和已观察到的 ASR 别名，final 前后各执行一次。
2. **解码热词偏置**：在 SenseVoice CTC 解码阶段使用前缀 beam search 和热词 trie，对候选路径加有限权重；必须保留无偏置候选，避免强行插入热词。
3. **短期术语上下文**：只传最近确认的专名或术语，不默认传完整对话；上下文仅作候选偏置，不得出现在输出中。
4. **LLM 可选后编辑**：用于标点、断句和低风险流畅度调整，不负责独立决定专有词，也不能覆盖受保护的代码、路径、URL、邮箱和用户词典词条。
5. **领域适配**：对固定领域的大量长尾词，优先考虑 SenseVoice 热词模型或领域微调，而不是不断扩大通用提示词。

当前 GGUF 后端使用 CTC greedy argmax，尚未实现 decoder-level 热词偏置。因此现阶段专有词能力主要来自规则词典，Qwen 仍默认关闭。
