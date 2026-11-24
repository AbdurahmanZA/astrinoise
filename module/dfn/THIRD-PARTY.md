# Third-party code in libastrinoise_dfn.so

Generated from `Cargo.lock` with `cargo metadata` (normal dependencies only; build-time tools and
proc-macros are not linked in). The DFN3 model is built into the library from the DeepFilterNet
repository (MIT OR Apache-2.0, Hendrik Schröter). The Apache-2.0 text is LICENSE-APACHE in this
folder; each crate's own repository has its copyright notice.

| Crate | Version | Licence |
|---|---|---|
| adler2 | 2.0.1 | 0BSD OR MIT OR Apache-2.0 |
| ahash | 0.7.9 | MIT OR Apache-2.0 |
| aho-corasick | 1.1.5 | Unlicense OR MIT |
| anyhow | 1.0.105 | MIT OR Apache-2.0 |
| bit-set | 0.5.3 | MIT/Apache-2.0 |
| bit-vec | 0.6.3 | MIT/Apache-2.0 |
| bitflags | 2.13.2 | MIT OR Apache-2.0 |
| byteorder | 1.5.0 | Unlicense OR MIT |
| bytes | 1.12.2 | MIT |
| cfg-if | 1.0.5 | MIT OR Apache-2.0 |
| const-random | 0.1.20 | MIT OR Apache-2.0 |
| crc32fast | 1.5.5 | MIT OR Apache-2.0 |
| deep_filter | 0.5.6 | MIT/Apache-2.0 |
| dlv-list | 0.5.2 | MIT OR Apache-2.0 |
| downcast-rs | 1.2.1 | MIT/Apache-2.0 |
| dyn-clone | 1.0.20 | MIT OR Apache-2.0 |
| either | 1.19.0 | MIT OR Apache-2.0 |
| errno | 0.3.15 | MIT OR Apache-2.0 |
| filetime | 0.2.30 | MIT/Apache-2.0 |
| flate2 | 1.1.11 | MIT OR Apache-2.0 |
| getrandom | 0.2.17 | MIT OR Apache-2.0 |
| half | 2.7.1 | MIT OR Apache-2.0 |
| hashbrown | 0.11.2 | Apache-2.0/MIT |
| hashbrown | 0.13.2 | MIT OR Apache-2.0 |
| itertools | 0.10.5 | MIT/Apache-2.0 |
| lazy_static | 1.5.1 | MIT OR Apache-2.0 |
| libc | 0.2.191 | MIT OR Apache-2.0 |
| libm | 0.2.16 | MIT |
| linux-raw-sys | 0.12.1 | Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT |
| log | 0.4.35 | MIT OR Apache-2.0 |
| maplit | 1.0.2 | MIT/Apache-2.0 |
| matrixmultiply | 0.3.11 | MIT/Apache-2.0 |
| memchr | 2.8.3 | Unlicense OR MIT |
| memmap2 | 0.5.10 | MIT OR Apache-2.0 |
| minimal-lexical | 0.2.1 | MIT/Apache-2.0 |
| miniz_oxide | 0.9.1 | MIT OR Zlib OR Apache-2.0 |
| ndarray | 0.15.6 | MIT OR Apache-2.0 |
| nom | 7.1.3 | MIT |
| num-complex | 0.4.6 | MIT OR Apache-2.0 |
| num-integer | 0.1.47 | MIT OR Apache-2.0 |
| num-traits | 0.2.19 | MIT OR Apache-2.0 |
| once_cell | 1.21.4 | MIT OR Apache-2.0 |
| ordered-multimap | 0.6.0 | MIT |
| ppv-lite86 | 0.2.21 | MIT OR Apache-2.0 |
| primal-check | 0.3.4 | MIT OR Apache-2.0 |
| prost | 0.11.9 | Apache-2.0 |
| rand | 0.8.8 | MIT OR Apache-2.0 |
| rand_chacha | 0.3.1 | MIT OR Apache-2.0 |
| rand_core | 0.6.4 | MIT OR Apache-2.0 |
| rand_distr | 0.4.3 | MIT OR Apache-2.0 |
| rawpointer | 0.2.1 | MIT/Apache-2.0 |
| realfft | 3.5.1 | MIT |
| regex | 1.13.2 | MIT OR Apache-2.0 |
| regex-automata | 0.4.19 | MIT OR Apache-2.0 |
| regex-syntax | 0.8.12 | MIT OR Apache-2.0 |
| rubato | 0.14.1 | MIT |
| rust-ini | 0.19.0 | MIT |
| rustfft | 6.4.1 | MIT OR Apache-2.0 |
| rustix | 1.1.6 | Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT |
| same-file | 1.0.6 | Unlicense/MIT |
| scan_fmt | 0.2.6 | MIT |
| serde | 1.0.229 | MIT OR Apache-2.0 |
| serde_core | 1.0.229 | MIT OR Apache-2.0 |
| simd-adler32 | 0.3.10 | MIT |
| smallvec | 1.16.6 | MIT OR Apache-2.0 |
| strength_reduce | 0.2.4 | MIT OR Apache-2.0 |
| string-interner | 0.14.0 | MIT/Apache-2.0 |
| tar | 0.4.46 | MIT OR Apache-2.0 |
| thiserror | 1.0.69 | MIT OR Apache-2.0 |
| tract-core | 0.19.16 | MIT/Apache-2.0 |
| tract-data | 0.19.16 | MIT/Apache-2.0 |
| tract-hir | 0.19.16 | MIT/Apache-2.0 |
| tract-linalg | 0.19.16 | MIT/Apache-2.0 |
| tract-nnef | 0.19.16 | MIT/Apache-2.0 |
| tract-onnx | 0.19.16 | MIT/Apache-2.0 |
| tract-onnx-opl | 0.19.16 | MIT/Apache-2.0 |
| tract-pulse | 0.19.16 | MIT/Apache-2.0 |
| tract-pulse-opl | 0.19.16 | MIT/Apache-2.0 |
| transpose | 0.2.3 | MIT OR Apache-2.0 |
| walkdir | 2.5.1 | Unlicense/MIT |
| xattr | 1.7.0 | MIT OR Apache-2.0 |
| zerocopy | 0.8.64 | BSD-2-Clause OR Apache-2.0 OR MIT |
| zlib-rs | 0.6.8 | Zlib |

## DeepFilterNet notice (MIT; also offered under Apache-2.0)

```
The MIT License (MIT)
Copyright (c) 2021 Hendrik Schröter

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```
