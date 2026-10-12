### attempt: div
| scenario | Mops/s before | Mops/s after | change | p50 ns before | p50 ns after | change |
|---|---|---|---|---|---|---|
| add | 22.8 | 22.9 | +1% | 98 | 98 | +0% |
| cancel | 15.2 | 18.1 | +19% | 175 | 165 | -6% |
| modify_reduce | 22.2 | 25.5 | +15% | 185 | 162 | -13% |
| modify_reprice | 10.0 | 9.1 | -9% | 177 | 203 | +15% |
| match_1 | 10.1 | 9.8 | -3% | 145 | 141 | -3% |
| match_4 | 2.2 | 2.5 | +13% | 399 | 380 | -5% |
| match_16 | 0.8 | 0.8 | +4% | 1242 | 1234 | -1% |
| match_64 | 0.2 | 0.2 | -3% | 4738 | 4671 | -1% |
| mixed | 16.5 | 17.0 | +3% | 150 | 142 | -6% |
| feed_replay | 18.2 | 18.5 | +2% | 149 | 147 | -1% |

### attempt: thp
| scenario | Mops/s before | Mops/s after | change | p50 ns before | p50 ns after | change |
|---|---|---|---|---|---|---|
| add | 18.0 | 20.6 | +14% | 149 | 105 | -29% |
| cancel | 14.6 | 13.5 | -7% | 208 | 217 | +4% |
| modify_reduce | 20.5 | 18.4 | -10% | 223 | 232 | +4% |
| modify_reprice | 7.9 | 7.8 | -1% | 223 | 220 | -1% |
| match_1 | 9.5 | 8.4 | -12% | 141 | 152 | +8% |
| match_4 | 2.2 | 2.0 | -9% | 414 | 476 | +15% |
| match_16 | 0.8 | 0.8 | -5% | 1210 | 1251 | +3% |
| match_64 | 0.2 | 0.2 | -3% | 4645 | 4793 | +3% |
| mixed | 16.9 | 16.1 | -5% | 152 | 155 | +2% |
| feed_replay | 17.7 | 17.2 | -3% | 155 | 159 | +3% |

### attempt: matchpf
| scenario | Mops/s before | Mops/s after | change | p50 ns before | p50 ns after | change |
|---|---|---|---|---|---|---|
| add | 16.2 | 17.8 | +10% | 154 | 131 | -15% |
| cancel | 13.1 | 14.5 | +11% | 223 | 222 | -0% |
| modify_reduce | 18.4 | 19.3 | +4% | 238 | 236 | -1% |
| modify_reprice | 7.2 | 6.4 | -11% | 242 | 232 | -4% |
| match_1 | 7.4 | 7.1 | -4% | 150 | 137 | -8% |
| match_4 | 2.2 | 1.9 | -13% | 459 | 504 | +10% |
| match_16 | 0.7 | 0.7 | -6% | 1240 | 1322 | +7% |
| match_64 | 0.2 | 0.2 | -1% | 4792 | 5083 | +6% |
| mixed | 15.7 | 15.2 | -3% | 154 | 155 | +1% |
| feed_replay | 15.7 | 16.2 | +4% | 160 | 159 | -1% |

### attempt: lochash
| scenario | Mops/s before | Mops/s after | change | p50 ns before | p50 ns after | change |
|---|---|---|---|---|---|---|
| add | 16.9 | 39.8 | +135% | 152 | 54 | -64% |
| cancel | 13.4 | 6.0 | -56% | 219 | 175 | -20% |
| modify_reduce | 18.3 | 28.0 | +53% | 235 | 157 | -33% |
| mixed | 15.3 | 21.3 | +39% | 157 | 65 | -59% |
| mixed_strided_ids | 16.6 | 17.4 | +5% | 149 | 104 | -30% |
| feed_replay | 16.1 | 12.0 | -26% | 159 | 76 | -52% |

### attempt: prefetch lookahead d4
| scenario | Mops/s before | Mops/s after | change | p50 ns before | p50 ns after | change |
|---|---|---|---|---|---|---|
| add | 18.5 | 15.8 | -15% | 107 | 158 | +48% |
| cancel | 16.2 | 13.3 | -18% | 195 | 214 | +10% |
| modify_reduce | 19.6 | 19.4 | -1% | 210 | 235 | +12% |
| modify_reprice | 8.4 | 6.0 | -28% | 192 | 247 | +28% |
| match_1 | 7.9 | 7.5 | -4% | 151 | 150 | -1% |
| match_4 | 2.5 | 2.1 | -18% | 386 | 436 | +13% |
| match_16 | 0.8 | 0.8 | +2% | 1263 | 1246 | -1% |
| match_64 | 0.2 | 0.2 | -1% | 4720 | 4799 | +2% |
| mixed | 16.6 | 16.2 | -2% | 151 | 153 | +1% |
| feed_replay | 15.9 | 16.3 | +3% | 155 | 160 | +3% |

### attempt: prefetch lookahead d8
| scenario | Mops/s before | Mops/s after | change | p50 ns before | p50 ns after | change |
|---|---|---|---|---|---|---|
| add | 18.5 | 17.7 | -5% | 107 | 148 | +38% |
| cancel | 16.2 | 13.8 | -15% | 195 | 210 | +7% |
| modify_reduce | 19.6 | 19.4 | -1% | 210 | 250 | +19% |
| modify_reprice | 8.4 | 6.8 | -19% | 192 | 228 | +18% |
| match_1 | 7.9 | 8.8 | +12% | 151 | 156 | +3% |
| match_4 | 2.5 | 2.1 | -17% | 386 | 510 | +32% |
| match_16 | 0.8 | 0.7 | -6% | 1263 | 1256 | -1% |
| match_64 | 0.2 | 0.2 | -1% | 4720 | 4774 | +1% |
| mixed | 16.6 | 16.2 | -3% | 151 | 155 | +3% |
| feed_replay | 15.9 | 15.5 | -3% | 155 | 160 | +3% |

### attempt: prefetch lookahead d16
| scenario | Mops/s before | Mops/s after | change | p50 ns before | p50 ns after | change |
|---|---|---|---|---|---|---|
| add | 18.5 | 16.8 | -10% | 107 | 150 | +41% |
| cancel | 16.2 | 13.4 | -18% | 195 | 203 | +4% |
| modify_reduce | 19.6 | 17.5 | -10% | 210 | 255 | +21% |
| modify_reprice | 8.4 | 7.0 | -16% | 192 | 235 | +22% |
| match_1 | 7.9 | 6.2 | -21% | 151 | 150 | -1% |
| match_4 | 2.5 | 2.0 | -20% | 386 | 514 | +33% |
| match_16 | 0.8 | 0.7 | -1% | 1263 | 1253 | -1% |
| match_64 | 0.2 | 0.2 | +4% | 4720 | 4648 | -2% |
| mixed | 16.6 | 16.0 | -4% | 151 | 155 | +3% |
| feed_replay | 15.9 | 15.7 | -1% | 155 | 159 | +2% |

### attempt: flags native
| scenario | Mops/s before | Mops/s after | change | p50 ns before | p50 ns after | change |
|---|---|---|---|---|---|---|
| add | 16.0 | 15.5 | -3% | 154 | 162 | +5% |
| cancel | 11.4 | 11.1 | -3% | 265 | 297 | +12% |
| modify_reduce | 18.5 | 15.5 | -16% | 246 | 262 | +7% |
| modify_reprice | 6.9 | 6.6 | -5% | 250 | 269 | +7% |
| match_1 | 7.1 | 7.5 | +6% | 143 | 158 | +11% |
| match_4 | 2.5 | 1.9 | -25% | 435 | 552 | +27% |
| match_16 | 0.8 | 0.7 | -6% | 1246 | 1287 | +3% |
| match_64 | 0.2 | 0.2 | -5% | 4764 | 4927 | +3% |
| mixed | 15.4 | 15.7 | +1% | 154 | 155 | +1% |
| feed_replay | 14.8 | 13.6 | -8% | 160 | 162 | +1% |

### attempt: flags lto
| scenario | Mops/s before | Mops/s after | change | p50 ns before | p50 ns after | change |
|---|---|---|---|---|---|---|
| add | 16.0 | 15.0 | -6% | 154 | 157 | +2% |
| cancel | 11.4 | 12.0 | +6% | 265 | 255 | -4% |
| modify_reduce | 18.5 | 17.4 | -6% | 246 | 261 | +6% |
| modify_reprice | 6.9 | 6.3 | -8% | 250 | 255 | +2% |
| match_1 | 7.1 | 6.2 | -12% | 143 | 174 | +22% |
| match_4 | 2.5 | 2.0 | -20% | 435 | 490 | +13% |
| match_16 | 0.8 | 0.7 | -1% | 1246 | 1254 | +1% |
| match_64 | 0.2 | 0.2 | -6% | 4764 | 4791 | +1% |
| mixed | 15.4 | 14.5 | -6% | 154 | 157 | +2% |
| feed_replay | 14.8 | 14.1 | -5% | 160 | 165 | +3% |

### attempt: flags o2
| scenario | Mops/s before | Mops/s after | change | p50 ns before | p50 ns after | change |
|---|---|---|---|---|---|---|
| add | 16.0 | 16.4 | +3% | 154 | 154 | -0% |
| cancel | 11.4 | 12.7 | +12% | 265 | 236 | -11% |
| modify_reduce | 18.5 | 16.6 | -10% | 246 | 254 | +3% |
| modify_reprice | 6.9 | 6.7 | -2% | 250 | 258 | +3% |
| match_1 | 7.1 | 6.3 | -11% | 143 | 188 | +31% |
| match_4 | 2.5 | 1.9 | -22% | 435 | 546 | +25% |
| match_16 | 0.8 | 0.7 | -14% | 1246 | 1270 | +2% |
| match_64 | 0.2 | 0.2 | +1% | 4764 | 4792 | +1% |
| mixed | 15.4 | 16.1 | +4% | 154 | 154 | -0% |
| feed_replay | 14.8 | 15.6 | +5% | 160 | 161 | +1% |

### attempt: flags clang
| scenario | Mops/s before | Mops/s after | change | p50 ns before | p50 ns after | change |
|---|---|---|---|---|---|---|
| add | 16.0 | 14.7 | -8% | 154 | 156 | +1% |
| cancel | 11.4 | 9.6 | -16% | 265 | 244 | -8% |
| modify_reduce | 18.5 | 17.8 | -4% | 246 | 257 | +5% |
| modify_reprice | 6.9 | 6.3 | -8% | 250 | 247 | -2% |
| match_1 | 7.1 | 7.6 | +7% | 143 | 153 | +7% |
| match_4 | 2.5 | 1.9 | -23% | 435 | 515 | +18% |
| match_16 | 0.8 | 0.7 | -3% | 1246 | 1288 | +3% |
| match_64 | 0.2 | 0.2 | -3% | 4764 | 4998 | +5% |
| mixed | 15.4 | 14.9 | -3% | 154 | 155 | +1% |
| feed_replay | 14.8 | 15.6 | +5% | 160 | 162 | +1% |

### attempt: id map group g1
| scenario | Mops/s before | Mops/s after | change | p50 ns before | p50 ns after | change |
|---|---|---|---|---|---|---|
| add | 19.2 | 18.2 | -5% | 125 | 97 | -22% |
| cancel | 13.5 | 12.5 | -8% | 217 | 204 | -6% |
| modify_reduce | 20.4 | 20.0 | -2% | 232 | 194 | -16% |
| modify_reprice | 7.5 | 7.3 | -3% | 231 | 227 | -2% |
| match_1 | 8.2 | 8.8 | +7% | 149 | 158 | +6% |
| match_4 | 2.2 | 2.3 | +6% | 398 | 432 | +9% |
| match_16 | 0.7 | 0.7 | -6% | 1241 | 1423 | +15% |
| match_64 | 0.2 | 0.2 | -19% | 4720 | 5566 | +18% |
| mixed | 15.5 | 15.9 | +2% | 155 | 136 | -12% |
| mixed_strided_ids | 17.9 | 19.1 | +7% | 143 | 104 | -27% |
| feed_replay | 17.0 | 16.5 | -3% | 157 | 144 | -8% |

### attempt: id map group g2
| scenario | Mops/s before | Mops/s after | change | p50 ns before | p50 ns after | change |
|---|---|---|---|---|---|---|
| add | 19.2 | 18.7 | -2% | 125 | 54 | -56% |
| cancel | 13.5 | 10.1 | -25% | 217 | 180 | -17% |
| modify_reduce | 20.4 | 22.3 | +9% | 232 | 216 | -7% |
| modify_reprice | 7.5 | 6.4 | -14% | 231 | 206 | -11% |
| match_1 | 8.2 | 8.2 | -1% | 149 | 163 | +10% |
| match_4 | 2.2 | 2.3 | +5% | 398 | 460 | +16% |
| match_16 | 0.7 | 0.6 | -20% | 1241 | 1642 | +32% |
| match_64 | 0.2 | 0.1 | -24% | 4720 | 6358 | +35% |
| mixed | 15.5 | 16.9 | +9% | 155 | 76 | -51% |
| mixed_strided_ids | 17.9 | 16.5 | -8% | 143 | 139 | -3% |
| feed_replay | 17.0 | 16.4 | -3% | 157 | 104 | -34% |

### attempt: id map group g3
| scenario | Mops/s before | Mops/s after | change | p50 ns before | p50 ns after | change |
|---|---|---|---|---|---|---|
| add | 19.2 | 24.5 | +28% | 125 | 55 | -56% |
| cancel | 13.5 | 8.0 | -41% | 217 | 179 | -18% |
| modify_reduce | 20.4 | 23.4 | +15% | 232 | 184 | -21% |
| modify_reprice | 7.5 | 5.6 | -25% | 231 | 215 | -7% |
| match_1 | 8.2 | 7.6 | -7% | 149 | 157 | +6% |
| match_4 | 2.2 | 2.1 | -4% | 398 | 469 | +18% |
| match_16 | 0.7 | 0.6 | -19% | 1241 | 1708 | +38% |
| match_64 | 0.2 | 0.1 | -27% | 4720 | 6691 | +42% |
| mixed | 15.5 | 18.3 | +18% | 155 | 70 | -55% |
| mixed_strided_ids | 17.9 | 15.9 | -12% | 143 | 150 | +5% |
| feed_replay | 17.0 | 16.4 | -4% | 157 | 86 | -45% |

### attempt: id map group g4
| scenario | Mops/s before | Mops/s after | change | p50 ns before | p50 ns after | change |
|---|---|---|---|---|---|---|
| add | 19.2 | 29.2 | +52% | 125 | 54 | -56% |
| cancel | 13.5 | 7.2 | -47% | 217 | 194 | -11% |
| modify_reduce | 20.4 | 25.4 | +24% | 232 | 158 | -32% |
| modify_reprice | 7.5 | 4.7 | -38% | 231 | 237 | +2% |
| match_1 | 8.2 | 6.8 | -17% | 149 | 177 | +19% |
| match_4 | 2.2 | 1.8 | -17% | 398 | 545 | +37% |
| match_16 | 0.7 | 0.5 | -34% | 1241 | 2030 | +64% |
| match_64 | 0.2 | 0.1 | -40% | 4720 | 7961 | +69% |
| mixed | 15.5 | 19.0 | +23% | 155 | 68 | -56% |
| mixed_strided_ids | 17.9 | 17.1 | -5% | 143 | 115 | -19% |
| feed_replay | 17.0 | 15.3 | -10% | 157 | 82 | -48% |

### A/A noise floor (same binary twice)
| scenario | Mops/s before | Mops/s after | change | p50 ns before | p50 ns after | change |
|---|---|---|---|---|---|---|
| add | 15.6 | 16.9 | +9% | 155 | 152 | -2% |
| cancel | 12.6 | 13.4 | +6% | 240 | 225 | -6% |
| modify_reduce | 19.2 | 21.4 | +11% | 256 | 226 | -12% |
| modify_reprice | 6.4 | 7.2 | +14% | 239 | 245 | +2% |
| match_1 | 7.6 | 7.2 | -5% | 153 | 151 | -1% |
| match_4 | 1.9 | 2.2 | +19% | 530 | 423 | -20% |
| match_16 | 0.8 | 0.8 | +0% | 1234 | 1209 | -2% |
| match_64 | 0.2 | 0.2 | +1% | 4813 | 4710 | -2% |
| mixed | 15.6 | 15.7 | +1% | 157 | 157 | -0% |
| mixed_strided_ids | 17.6 | 17.3 | -2% | 151 | 151 | -0% |
| feed_replay | 15.7 | 16.4 | +5% | 161 | 160 | -1% |
