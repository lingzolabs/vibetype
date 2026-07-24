# Text-polishing synthetic stress set

`synthetic-stress.jsonl` contains deliberately difficult, hand-written cases for
regression and safety testing. It is **not** representative of SenseVoice's
normal error distribution and must not be used to report ASR quality.

Use the real-audio FLEURS manifest and `tools/benchmark_real_asr.py` for the
primary baseline-versus-polishing comparison. Keep prompt examples disjoint
from both datasets.
