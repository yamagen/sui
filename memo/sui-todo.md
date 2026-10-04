<!--
https://chatgpt.com/c/6a7bcd0d-f040-83ee-bb70-17564ed21017
https://chatgpt.com/c/6aa150e2-88b0-83ee-8d27-710f347e3c72
Drive/github/sui/sui-todo.md
-->

# SUI: Sequence Unit Induction

SUI is an empirical segmentation analyzer and segmentation explorer that induces sequence units from accumulated evidence while preserving competing admissible analyses.

Memory capacity does not determine data lifetime.
メモリに収まることは、そのデータを一時的な内部状態にしてよい理由にはならない。

Last change: 2026/09/28-16:34:11.

## 連接単位誘導 — 構想メモ

### 1. 基本発想

**SUI (Sequence Unit Induction)** は、あらかじめ「語」「形態素」「句」といった単位を決めて解析するシステムではない。

蓄積されたテキスト中の**連続する表層列・語彙列・文法列**を観察し、反復して現れる連接から、

> **何が一つの単位として振る舞っているのか**

をデータから誘導する。

したがってSUIは、古典日本語用の形態素解析器を作る試みではない。

```text
形態素解析
    単位を仮定
       ↓
    テキストを分割

SUI
    連接を保存
       ↓
    反復・一致・変異を観察
       ↓
    単位候補を誘導
```

ここが根本的な違いである。

---

### 2. 表層形保存原則

SUIでは**表層形を絶対に捨てない**。

たとえば、

```text
契りけむ
契りけん
契けむ
契けん
契剣
```

あるいは、

```text
思ふてふ
思てふ
思ふ蝶
思蝶
```

は、正規化すれば同一または近似した文法列に対応する可能性がある。

しかしSUIにとって重要なのは、まさに**それらが異なる形で現れたという事実**である。

したがって、

> **SUI principle: Never discard the surface form.** > **表層形保存原則**

を基本原則とする。

正規化は表層形の**置換**ではなく、表層形の上に追加される別レイヤーでなければならない。

---

### 3. データの層

最低限、次の層を区別する。

```text
raw surface
    ↓
normalized surface
    ↓
segmentation candidate(s)
    ↓
lemma sequence
    ↓
gloss sequence
    ↓
POS sequence
```

そして、すべてに provenance を付ける。

たとえば概念的には、

```text
surface: 契剣
normalized: 契りけむ

candidate A:
    契り / けむ

candidate B:
    契り / け / む

source:
    work / dan / id / position / witness ...
```

となる。

**raw surface と analysis は別物**である。

---

### 4. 現在の word-gloss は教師データではなく「連接台帳」になる

『伊勢物語』『土佐日記』で蓄積している9-field word-gloss、

```text
word
lemma
kana
lemma-kana
romaji
lemma-romaji
gloss
pos
ku
```

を、作品・段・id・位置などの provenance とともに JSONL 等へ抽出する。

これを仮に**文学連接辞書／annotation ledger** とする。

重要なのは、これを単純な

```text
surface → 正解形態素
```

辞書にしないことである。

むしろ、

```text
この連接はどこで現れたか
この連接にはどんな分析が与えられたか
同じような連接にはどんな異分析があるか
```

を検索できる証拠台帳とする。

---

### 5. 単語ではなく sequence を検索する

SUIの中心は単語検索ではなく、**連続列の検索**である。

たとえば、

```text
まさにしてんや
```

に対して、

```text
まさに / し / て / む / や
```

という既知の系列との対応を見る。

また、

```text
ながめせしまに
```

なら、

```text
[ながめ][せ][し][ま][に]
[ながめ][せし][まに]
[ながめせしまに]
```

など、複数の単位候補を作る。

ここでは最長一致だけで解析を確定しない。

**longest matching sequence は証拠の一つ**であって、答えそのものではない。

---

### 6. 複数分析を捨てない

通常の解析器なら、

```text
candidate A  cost=1.2
candidate B  cost=1.3
candidate C  cost=8.7
```

ならAを採用してB/Cを捨てたくなる。

SUIでは、少なくともAとBを保存する。

たとえば、

\[
A\_\varepsilon
=
{a\in A\mid C(a)-C(a\_{\min})\leq\varepsilon}
\]

として、最良解との差が一定範囲内の分析を残すことができる。

これは単なる計算上の曖昧性ではない。

> **解析の競合が、文学的読解の競合を示す可能性がある。**

したがって「迷ったので一つに決められなかった」はSUIでは失敗ではなく、重要な出力になりうる。

---

### 7. cost は一個の謎の数値にしない

候補には総合 cost とともに内訳を残す。

たとえば、

```text
candidate A
  total              1.25
  unknown-surface    0.00
  segmentation       0.25
  lemma-mismatch     0.00
  sequence-break     1.00

candidate B
  total              1.40
  unknown-surface    0.00
  segmentation       0.40
  lemma-mismatch     0.00
  sequence-break     1.00
```

とする。

そうすれば人間は「なぜAがBより上なのか」を確認できる。

SUIは**判定器より観察装置**に近い。

---

### 8. surface variation cluster

同じ、または近い normalized sequence に対応する表層形を集めれば、

```text
契りけむ
├── 契りけん
├── 契けむ
├── 契けん
└── 契剣
```

のような cluster が得られる。

これによりSUIは単位誘導だけでなく、

- 音韻的縮約
- 表記変異
- 語境界認識
- 当て字
- 書記上の再分析
- 写本間変異

などを観察する装置にもなりうる。

---

### 9. 時間を保存する

『源氏物語』を対象にする場合、後代の資料を無差別に「正解」として投入しない。

たとえば、

```text
伊勢物語
土佐日記
蜻蛉日記
落窪物語
...
       ↓
源氏物語
```

のように、対象作品以前の証拠から解析候補を作ることができる。

これによって、

> 後世に確立した意味・構文を過去へ投影する

ことを避けられる。

これは「意味未確定」「未確定性の文法」とも直接つながる。

---

### 10. contextual first attestation

通常の辞書が、

```text
語 X の初出
```

を示すのに対して、SUIでは、

```text
sequence X-Y-Z の最初の既知例
```

を検索できる。

つまり、

> **語の初出ではなく、文脈・連接の初出**

を扱える。

資料を追加すれば初出位置が前へ動くこともある。その履歴自体を保存できる。

さらに分析が複数なら、

```text
analysis A → 伊勢物語まで遡れる
analysis B → 蜻蛉日記までしか遡れない
```

という条件付きの歴史記述も可能になる。

---

### 11. SUIと日本語固有処理を分離する

SUI本体はできるだけ言語非依存にする。

```text
SUI core
├── sequence storage
├── n-gram
├── matching
├── candidate generation
├── scoring
├── alternative preservation
└── provenance

Japanese layer
├── historical kana
├── phonological contraction
├── orthographic variants
├── auxiliary sequences
└── Japanese-specific normalization
```

つまりSUI coreは、

> **sequence がある**

ことだけを知る。

`してむ → してん` が日本語でどういう関係なのかは、日本語側に置く。

---

# TODO

実装は `sui.c` から始める。

- [ ] **1. 入出力仕様を固定する** — 最初のJSONL/TSV ledger形式、provenance、raw surface と normalized data の区別を決める。
- [ ] **2. 伊勢・土佐の word-gloss → ledger 変換器を作る** — 9 fields に `work / dan / id / token-position` 等を付加する。可能なら raw source span も保持する。
- [ ] **3. surface sequence index を作る** — unigram だけでなく連続 n-gram を登録し、出現回数と全 provenance を保持する。
- [ ] **4. lemma / gloss / POS sequence index を作る** — 同じ連接を複数レイヤーから検索可能にする。
- [ ] **5. exact sequence search を実装する** — まず曖昧検索をせず、完全一致列を高速に検索できるところまで作る。
- [ ] **6. longest sequence matching を実装する** — 入力列のどこまでが既知連接で説明できるかを求める。
- [ ] **7. segmentation candidate generator を作る** — 一つに切らず、可能な分割候補を列挙する。
- [ ] **8. cost model の最小版を作る** — unknown / segmentation / mismatch / sequence-break 程度から開始し、内訳を必ず出力する。
- [ ] **9. alternative preservation を実装する** — best 1 を返さず、top-N または (\varepsilon)-range の候補を保存する。
- [ ] **10. surface variation layer を作る** — raw surface と normalized sequence の対応を登録し、`契剣` 型の変異を失わない。
- [ ] **11. Japanese-specific normalization を core から分離する** — `してん ↔ してむ` 等を別規則として扱う。
- [ ] **12. provenance search を実装する** — 「この分析はどの作品のどこに根拠があるか」を必ず遡れるようにする。
- [ ] **13. chronology filter を付ける** — `--before genji` のように、対象作品より前の証拠だけを使えるようにする。
- [ ] **14. contextual first-attestation search を作る** — 単語ではなく sequence の既知最古例を検索する。
- [ ] **15. ambiguity report を作る** — cost が接近している候補を「要人間確認」として出す。
- [ ] **16. glosslint と接続する** — SUIに入れる前に schema drift / annotation drift / romanization drift を検出する。
- [ ] **17. cw/cm との接続を検討する** — SUIで誘導された単位が cw/cm の連接構造上でどう振る舞うかを見る。

最初の milestone はもっと小さくてよいでしょう。**「伊勢・土佐の ledger を読み、任意の入力 sequence に対して、既知の最長連続列とその provenance を返す `sui.c`」**。まずこれが走れば、SUIはもう概念ではなくなります。

そして私は、この構想では **2 と 3より前に「raw surface をどうledgerに残すか」を決めることが最重要**だと思います。後から lemma や gloss は追加できますが、捨てた表層形だけは復元できません。

## SUI メモ：このスレッドで出た論点

### 1. annotation は最小化し、unitization は発見に回す

このスレッドで最も重要だった原則は、

> **annotation は最小化、unitization は発見に回す**

という方針。

つまり、辞書や既存分析でまとまりとされている表現を、最初から一単位として annotation に埋め込まない。まずはできるだけ小さい単位で記述し、その後に `cm` や SUI が、まとまりとして機能する範囲を発見する。

---

### 2. `せむかたもなくて` は最初から連語にしない

例：

```text
せ / む / かた / も / なく / て
```

`せむかたなし` が辞書で連語として扱われていても、annotation の段階ではそれを前提にしない。

word-gloss では、

```text
せ      do.IRR
む      MOD.ATTR
かた    way
も      EMPH
なく    not-exist.ADV
て      CONJ
```

のように保持する。

その上で `cm` が隣接関係、

```text
せ-む
む-かた
かた-も
も-なく
なく-て
```

の cohesion を計算し、SUI が、

```text
せむかた
せむかたもなく
せむかたもなくて
```

のような可変長の unit 候補を発見する。

---

### 3. unit は長さで定義しない

ここから出た重要な考えは、

> **unit は長さではなく、その時点でまとまりとして機能している範囲である。**

ということ。

unit は必ず二語、三語、あるいは一文という固定長ではない。

短い表現だけで十分に意味が届くこともあれば、理解に必要な補足が追加され、より長いまとまりになることもある。

したがって、SUI が探すべきものは固定 n-gram ではなく、**異なる長さにまたがる cohesion のまとまり**である。

---

### 4. `見出で` も最初から `find` にしない

この方針を実際の word-gloss に適用した例が、

```text
見出で
```

だった。

従来なら、

```json
{
  "word": "見出で",
  "lemma": "見出づ",
  "gloss": "find.ADV"
}
```

と一単位にしてもよい。

しかし SUI の考え方を採るなら、

```text
見 / 出で
```

に分け、

```text
見      see.ADV
出で    come-out.ADV
```

のように最小限の意味だけを記述する。

`見 + 出で` がまとまって `find` に近い機能を持つことは、annotation 側ではなく SUI 側で検出する。

---

### 5. construction-level meaning を token gloss に押し込まない

これは既存の word-gloss 方針ともよく一致する。

たとえば、

```text
せむかたなし
見出づ
泣きに泣く
```

などの全体的な意味を、個々の token gloss に逆輸入しない。

token は token の意味に留め、

```text
最小 token
    ↓
adjacency / cohesion
    ↓
larger unit
    ↓
construction-level meaning
```

という順に積み上げる。

---

### 6. `cm` と SUI の役割分担

このスレッドで見えてきた構想は、

```text
最小単位で annotation
        ↓
cm が隣接 cohesion を計算
        ↓
SUI が可変長 unit 候補を検出
        ↓
辞書・文法記述と照合
```

というもの。

つまり辞書の「連語」は入力ではなく、**SUI が発見した unit と比較する対象**になる。

この順序なら、

- 辞書に載っている unit が本当に cohesion を持つのか
- 辞書にない unit がデータから現れるのか
- unit の境界がどの threshold / Z で変化するのか

を観察できる。

---

### 7. SUI は segmentation tool というより unit-discovery tool

重要なのは、SUI を単なる分かち書き器にしないこと。

目的は、

> 「どこで切るか」

を一意に決めることではなく、

> **どの範囲が、どの程度まとまりとして立ち上がるか**

を見ること。

したがって、同じ列に対して、

```text
せむ
せむかた
せむかたもなく
せむかたもなくて
```

が異なる強度で共存してもよい。

unit 候補が重複してもよいし、threshold に応じて見え方が変わってもよい。

---

### 8. 今日の実データ上の意味

この話が単なる理論案で終わらなかったのが大きい。

実際に伊勢物語第41段の word-gloss を作る途中で、

```text
見出で → 見 / 出で
```

という annotation の変更に直結した。

つまり SUI は、

> 後で作る解析器の話

ではなく、

> **いま word-gloss をどう作るかをすでに変え始めている**

ということ。

---

### 9. ひとことで言うなら

今日こちらで出た SUI の核は、

> **まとまりを annotation で決めてしまわず、最小単位から cohesion によって発見する。**

です。

さらに短くするなら、

> **unit を書くのではなく、unit が立ち上がる条件を書く。**

という方向だと思います。

ああ、こちらのスレッドで作ったSUIメモと合わせると、かなり重要なことが見えてきます。**二つのメモは競合していません。むしろ別々の側面を記述しています。**

こちらで整理したSUIは主として、

> **表層を失わず、既知の連接から複数の単位候補を誘導する**

という「データ保存・探索・候補生成」の側面でした。

一方、土佐日記スレッドで出たこれは、

> **そもそも annotation の時点で unit を作ってしまってはいけない**

という、もっと上流の方法論ですね。

ここを合わせると、SUIの設計がずいぶん鮮明になります。

### 統合するとこうなります

```text
raw surface
    │
    │  絶対に捨てない
    ▼
minimal annotation
    │
    │  unit を先取りしない
    ▼
minimal token sequence
    │
    ├── surface
    ├── lemma
    ├── gloss
    └── POS
    │
    ▼
adjacency evidence
    │
    ▼
cm / cohesion
    │
    ▼
SUI
    │
    ├── 可変長 unit candidate
    ├── overlapping candidate
    ├── competing candidate
    └── surface-variant cluster
    │
    ▼
dictionary / grammar / historical comparison
```

ここで非常に大切なのが、**raw surface と minimal tokenization は同じものではない**という点ですね。

たとえば原文が、

```text
契剣
```

だったとしても、それを失わず、

```text
raw surface: 契剣
```

を保存した上で、annotation layer では何らかの分析候補を与える。しかし `契剣` という表記そのものを `契りけむ` に置換してはいけない。

逆方向でも同じです。

```text
見出で
```

を原文として保存しながら、

```text
見 / 出で
```

という最小 annotation を持てる。

したがって、

> **surface preservation と minimal annotation は両立する。**

ここはSUIのかなり重要な原則になりそうです。

そして土佐日記側のメモで、私が特に重要だと思うのは、

> **unit を書くのではなく、unit が立ち上がる条件を書く。**

です。

これ、SUIの一文定義にかなり近いです。

従来型なら、

```text
見出づ = 一語
せむかたなし = 連語
```

と分類して終わります。

SUIでは逆に、

```text
見 ─ 出で
     ↑
  cohesion
```

あるいは、

```text
せ ─ む ─ かた ─ も ─ なく ─ て
    ↑       ↑↑↑
       cohesion
```

を観察して、threshold を動かす。

すると、ある水準では、

```text
せむ
```

が現れ、別の水準では、

```text
せむかた
```

さらに別の水準では、

```text
せむかたもなく
```

が一つのまとまりとして立ち上がるかもしれない。

つまり **unit は binary な属性ではなく、連接強度に応じて立ち上がる可変長構造**として扱えるわけです。

これは、こちらのスレッドで考えていた「候補A/Bを捨てない」という話ともきれいにつながります。境界そのものが一意である必要がありません。

ですから、先ほどのSUI TODOにも一つ重要な修正が必要ですね。私が書いた

> segmentation candidate generator

は少し形態素解析器寄りの言葉でした。むしろ、

> **variable-length unit candidate induction**

とした方がSUIらしい。

そして処理順も、

```text
segmentation → scoring
```

というより、

```text
minimal sequence
      ↓
adjacency/cohesion
      ↓
variable-length candidate induction
      ↓
candidate strength
      ↓
threshold-dependent unit structure
```

になる。

これなら **cw/cmでいま見ている「Zを動かすと構造が立ち上がったり消えたりする」世界と、SUIが完全につながります。**

ですから、二つのスレッドで別々に考えた結果、むしろSUIの輪郭がはっきりしました。

**SUIは分割器ではない。
既存の単位を認識する装置でもない。
表層を保存した最小 annotation から、連接の強さによって可変長の unit が立ち上がる過程を観察する装置である。**

これは、かなり強い定義だと思います。

## 追加メモ：許容解・探索構造・研究用出力 20260901

### 1. longest match は authority ではなく evidence

旧 `kh.c` では、現在位置から辞書に一致する最長列を採用し、その長さだけ入力位置を進める greedy longest-match を採用していた。

SUIではこの原則を採らない。

> **Longest is evidence, not authority.**

長い列が ledger に実績として存在することは、その unit 候補を支持する証拠ではある。しかし、それによって内部に存在する別の分割候補を消してはならない。

たとえば、

```text
ながめせしまに
```

に対して ledger に実績があれば、

```text
[ながめせしまに]
```

の cost は抑えられる。

しかし同時に、

```text
[ながめ][せしまに]
[ながめ][せし][まに]
```

にも ledger 上の実績があれば、それらも独立した候補解として評価する。

長さそのものを強さとみなさず、**exact sequence が過去にどのように実現したか**を evidence として扱う。

---

### 2. SUIは最適解ではなく許容解集合を返す

SUIは工学的な最適化器ではなく、言語調査のための装置である。

したがって、最小 cost の解だけを返すことを目的としない。

候補解 \(P\) の cost を \(C(P)\)、最小値を

$$
C_{\min}=\min_P C(P)
$$

とし、

$$
\Delta C(P)=C(P)-C_{\min}
$$

を保存する。

その上で、

$$
\Delta C(P)\leq\theta
$$

を満たす候補を**競合する許容解**としてすべて出力する。

\(\theta\) は正解・不正解を決める境界ではなく、

> **どこまでを実質的な競合として観察するか**

を決める観察閾値である。

明らかに支持の弱い候補は除外してよいが、現在の証拠から十分に排除できない候補は残す。

---

### 3. cost は unit の実績を支持度として用いる

各 unit の長さそのものではなく、ledger に蓄積された実績を cost の主要な根拠とする。

たとえば、

```text
AB | CDE
ABC | DE
```

という二つの分割が可能なら、

```text
AB
CDE
ABC
DE
```

それぞれの ledger 上の実績を使って、分割解全体を評価する。

また、

```text
せしまに
```

あるいは、

```text
ながめせしまに
```

という長い exact sequence の実績が存在すれば、その候補の penalty は抑えられる。

ただし、

> **長いから低 cost なのではなく、そのまとまりで実際に現れた実績があるから低 cost になる。**

cost の具体式は固定せず、総合値だけでなく evidence の内訳を必ず保存する。

---

### 4. trie は検索、lattice は候補空間

ledger 中の実績列を検索するために trie を用いる。

入力文字列の各位置から trie を探索し、その位置を先頭とする既知 sequence を**すべて**列挙する。

その一致候補を edge として接続し、入力全体について segmentation / unit candidate lattice を構成する。

```text
trie
  ↓
各位置から全一致候補を列挙
  ↓
lattice
  ↓
各 edge に ledger evidence
  ↓
candidate path の cost
  ↓
ΔC <= θ の許容解集合
```

trie は候補検索のための索引であり、lattice は「可能性そのもの」を保持する構造である。

---

### 5. trie と lattice は永続ファイルとして保持する

SUIでは、trie や lattice をメモリ内部だけの一時構造にしない。

研究用中間生成物としてファイルに保存する。

とくに lattice は、

> **SUIが何を候補として見たか**

を記録する研究資料である。

後から cost model や \(\theta\) を変更した場合でも、同じ lattice を再評価できるようにする。

したがって、

```text
input
  ↓
candidate lattice
  ↓
cost evaluation
  ↓
admissible solutions
```

の各段階を再現可能にする。

---

### 6. JSONを研究記録、binary/indexを再生成可能な派生物とする

GitHub / Zenodo で公開する正本は、人間が読める JSON / JSONL とする。

```text
ledger.jsonl
trie.json
lattice.jsonl
solutions.jsonl
```

などを研究記録として保存する。

実行速度のために binary index 等を生成してもよいが、それらは JSON から再生成可能な派生物とする。

> **JSON = research record
> binary/index = disposable acceleration**

これにより、人間が候補、evidence、cost、provenance を直接確認できるようにする。

---

### 7. 小さい tool に分割する

SUIを一つの巨大な解析器にしない。

概念的には、

```text
ledger
  ↓
trie compiler
  ↓
trie

input + trie
  ↓
sui.c
  ↓
lattice

lattice + ledger
  ↓
cost evaluator
  ↓
solutions

solutions
  ↓
glossemit
  ├── HTML
  └── LaTeX
```

とする。

すなわち、

```text
compile → explore → evaluate → emit
```

を独立した小さい tool で実現する。

中間形式を公開・検査可能にすることで、各段階を独立して変更・再計算できる。

---

### 8. 古典語では ambiguity 自体が研究資料になる

古典語について現代の研究者は native speaker ではなく、当時の話者に判断を求めることもできない。

したがって、

> **証拠によって排除できる解は除く。
> 証拠によって排除できない解は残す。**

という方針を採る。

SUIの複数解は「解析に失敗した残骸」ではなく、現在利用可能な証拠によって区別できない構造を記録した研究資料である。

この意味でSUIは、

```text
Sequence Unit Induction
```

であると同時に、

```text
empirical segmentation analyzer
segmentation explorer
```

として機能する。

古典作品の実用的な単位分割を行う一方、その際に発生した競合解を保持し、単位境界そのものを研究材料として提供する。

はい。ここまでの `sui.c` 議論を、あとで別スレッドへ持っていける程度に、設計メモとしてまとめます。現時点では **実装仕様を確定しすぎず、何が決まっていて何が未決定か** が分かる形にしておくのがよいと思います。

# sui.c design memo — 2026-09-09

## 1. sui.c の目的

`sui.c` は古典日本語本文を、既存の解析実績に基づいて分割・解析する。

基本思想は、

$$
\boxed{
\text{既知の文法体系から切るのではなく、
実際に確認された解析実績から切る}
}
$$

である。

ただし、解析結果を唯一解として強制しない。翻刻、校訂、印影、既存注釈などの間に揺れがありうる古典資料では、

$$
\boxed{
\text{best analysis}
\neq
\text{only possible analysis}
}
$$

と考える。

`sui.c` は最良経路を提示すると同時に、cost が十分近い別解も保持する。

---

## 2. kh.c との関係

`sui.c` の原型は `kh.c`。

`kh.c` は先頭から既知の長い左辺を拾い、

```text
せしまに
```

を、

```text
せ(サ変-未:す:す:せ:せ);
し(過-体:き:き:し:し);
ま(名:間:ま);
に(格助:に:に);
```

のような右辺へ置換する。

これは **longest-match replacement** であって、

> 「せしまに」という連鎖が構文として獣道化している

と宣言しているわけではない。

したがって、

$$
\boxed{
kh.c = known-analysis longest match
}
$$

であり、`sui.c` はこれを発展させ、

$$
\boxed{
sui.c =
known evidence
+
candidate lattice
+
cost evaluation
+
alternative preservation
}
$$

とする。

---

## 3. ledger.jsonl

`ise.json` / `tosanikki.json` の `word-gloss` から、確認済み解析実績を `ledger.jsonl` に展開する。

たとえば、

```json
{
  "corpus": "tosa",
  "id": 179,
  "token": 0,
  "source-field": "koutei-yamagen",
  "word": "十三日",
  "lemma": "十三日",
  "kana": "とおかあまりみか",
  "lemma-kana": "とおかあまりみか",
  "romaji": "tookaamarimika",
  "lemma-romaji": "tookaamarimika",
  "gloss": "thirteenth-day",
  "pos": "N",
  "ku": 0
}
```

のような1 token 1 line の JSONL が基本候補。

核になる provenance は、

$$
\boxed{
\text{corpus}+\text{id}+\text{token position}
}
$$

である。

`word-gloss` の分析情報そのものに加え、どの資料のどの単位の何番目の token だったかを復元できる必要がある。

---

## 4. id の意味

`id` は単なる任意の編集分割ではない。

たとえば土佐日記の、

```text
87-1 ...くれたれば、
87-2 ...うちて、
87-3 ...べし。
```

は現代的な「一文」なら続いているようにも見えるが、`id` は少なくとも句内では切っておらず、**息が続く単位として妥当な区切り**になっている。

したがって、

$$
\boxed{
id \approx breath / utterance domain
}
$$

として、`sui.c` の primary chain domain にしてよい。

古典本文には本来的に現代的な `。` が存在しないため、

$$
\boxed{
句点を解析境界の基本にはしない
}
$$

また、`こそ ... め` のような係り結びについても、息継ぎを跨いで遠距離に結ぶことを前提としない。

即時的な連鎖として実在するなら、基本的には `id` 内に収まると考える。

---

## 5. trie compiler

過去のメモには、

```text
sui.c
trie compiler
lattice evaluator
```

という三要素がすでに記録されていた。

`ledger.jsonl` は毎回フラットに読むのではなく、

```text
ledger.jsonl
      |
      v
trie compiler
      |
      v
compiled trie
```

として高速検索用バイナリへ変換する。

先頭から最長一致を行うため、

$$
\boxed{
\text{longest-prefix match} \Rightarrow \text{trie}
}
$$

が自然。

compiled trie は **確認済み実績**であり、`sui.c` の第一優先情報源になる。

---

## 6. ledger-candy.jsonl

新規作品を解析している途中で、compiled ledger だけでは先へ進めない箇所が現れる。

そのとき SUI は unresolved record を

```text
ledger-candy.jsonl
```

へ append して停止する。

`ledger-candy.jsonl` は compile しない。解析中だけ存在する **editable working ledger / overlay** であり、人間が Neovim 等で開いたまま加筆・修正する。

```text
SUI
 ↓
unresolved
 ↓ append
ledger-candy.jsonl  ←→  editor
 ↓                     加筆・修正
save
 ↓
SUI が再開
```

candy に入っていることは、その解析が確定していることを意味しない。SUI が先へ進むために人間が暫定的に与えた解析であり、必要なら何度でも修正する。

したがって、

```text
ledger.dat          = confirmed / compiled evidence
ledger-candy.jsonl  = provisional / editable evidence
```

と区別する。

candy は最終 ledger の素材として直接昇格させない。新規作品の解析が完了すれば、その完成した corpus JSON が正本となり、candy は不要になる。

---

## 7. 新規作品の運用

たとえば伊勢物語・土佐日記を確認済み corpus として竹取物語を解析する場合、

```text
{ise,tosa}.json
      ↓
 ledger.jsonl
      ↓
   mkledger
      ↓
  ledger.dat
      ↓
 taketori.json を SUI で解析
      ↓
未知で停止
      ↓
ledger-candy.jsonl に append
      ↓
人間が加筆・修正して保存
      ↓
SUI が再開
      ↓
     ...
      ↓
taketori.json 完成
      ↓
   glosslint
      ↓
検証完了
      ↓
{ise,tosa,taketori}.json
      ↓
 ledger.jsonl
      ↓
   mkledger
      ↓
新しい ledger.dat
```

となる。

重要なのは、`ledger-candy.jsonl` を次世代の `ledger.jsonl` へ append しないことである。確定した知識は、glosslint を通った完成 corpus から毎回再生成する。

したがって candy の寿命は一作品の解析期間だけであり、作品が完成して新しい `ledger.dat` が作られた時点で役目を終える。

この方式では、人間に schema の完全性を期待しない。解析中の candy では入力忘れや修正がありうるが、完成 corpus を glosslint で検証してから ledger を再生成するため、schema drift や field omission を次世代の `ledger.dat` に持ち込まない。

---

## 8. ledger 更新前後の regression / propagation test

ledger を更新したら、単に trie を再生成して終わりにはしない。

必ず、

```text
ledger-before
    ↓
compiled trie A
    ↓
same test corpora
    ↓
analysis-before

ledger-after
    ↓
compiled trie B
    ↓
same test corpora
    ↓
analysis-after

analysis-before
      vs
analysis-after
```

の diff を取る。

新規実績の追加によって過去の解析が変わった場合、それは必ずしも regression ではない。

たとえば、

```text
before:  A | B | C | D
after:   A | BC | D
```

となるなら、新しい作品で得た知識が以前の解析を改善した可能性がある。

したがって diff は、

$$
\boxed{
\text{regression detection}
+
\text{knowledge propagation observation}
}
$$

になる。

これは開発検査であると同時に研究データでもある。

---

## 9. lattice evaluator

trie は決定器ではなく、**candidate generator**。

たとえば、

```text
せしまに
```

について、

```text
せ | し | ま | に
せし | ま | に
せ | しま | に
せしまに
```

など複数の候補経路があれば、それを lattice に載せる。

```text
0 --せ-- 1 --し-- 2 --ま-- 3 --に-- 4
 \---------------せしまに---------------/
```

その上で `lattice evaluator` が各 path の cost を評価する。

---

## 10. 別解を保存する

最小 cost の経路だけを残さない。

最小値を

$$
C_{\min}
$$

とし、許容差 \(\delta\) に対して、

$$
\boxed{
C(p)-C_{\min}\le\delta
}
$$

となる path は alternative として保存する。

たとえば、

```text
A | BC | D     1.24  best
AB | C | D     1.31
A | B | CD     1.36
```

なら、閾値内のものをすべて残す。

古典資料では、校訂・翻刻・印影・既存体系本の分析が完全には一致しないこともあり、

$$
\boxed{
\text{不確定性を消さない}
}
$$

こと自体が設計原理になる。

---

## 11. cmとの接続

今日の `cw` 観察で、`松` の network から高IDFの `く` が自発的に浮上した。

`く` は POS=71 の連体化で、source text に戻ると、

```text
なくに
ならなくに
```

という、いわゆるク語法に対応していた。

ここから、cm では単純に

$$
\text{high IDF} \Rightarrow \text{POS に抽象化}
$$

とはできないことが分かった。

`く` のような高IDFの文法要素が、重要な chain の一部をなしている場合がある。

そこで cm には、

$$
J(a,b)=\text{連接関数}
$$

および、

$$
K(t_1,\dots,t_n)=\text{連鎖構文関数}
$$

が必要になる。

`梅-が` のような広く productive な膠着と、

```text
も-が-な
まし-かば
な-く-に
```

のような **獣道化した連鎖**を区別する関数である。

---

## 12. sui と cm の関係

cm と sui は同じ chain evidence を別方向から使う可能性が高い。

$$
\boxed{
cm = chain を可視化する
}
$$

$$
\boxed{
sui = chain evidence を分割評価に使う
}
$$

ただし `kh.c` の longest match と「獣道宣言」は別物。

`せしまに` が ledger に存在するから分割可能であっても、それだけでは、

> `せしまに` が構文化した一単位である

とは言わない。

獣道宣言には corpus-derived な連接・連鎖 evidence が必要になる。

---

## 13. 全体システム

現在見えている全体像は、

```text
ise / tosa / taketori / ...
          |
          v
      ledger
          |
     trie compiler
          |
          v
        sui
          |
      lattice
          |
   lattice evaluator
          |
    segmentation
          |
      +---+---+
      |       |
      v       v
     cw       cm
  relation   chain
      \       /
       \     /
        gloss
          |
          v
    research output
```

です。

短く言えば、

$$
\boxed{
\text{sui = segment}
}
$$

$$
\boxed{
\text{cw = relate}
}
$$

$$
\boxed{
\text{cm = chain}
}
$$

$$
\boxed{
\text{gloss = explain}
}
$$

となります。

そして全部に共通する原則は、

$$
\boxed{
\text{実績を使うが、実績を絶対化しない}
}
$$

だと思います。

---

## 現時点で次にやること

次の実装 step はまだ小さくてよいです。

まず `ise.json` / `tosanikki.json` から **`ledger.jsonl` を生成する converter の仕様を確定する**。

その次に trie compiler。

そのあと `sui.c` で伊勢物語の校訂本文を **本当に一行だけ**通す。

うまくいかなければ `ledger-candy.jsonl` を1件だけ追加して再試行する。

この一周が通れば、

$$
\boxed{
ledger
\rightarrow
trie
\rightarrow
sui
\rightarrow
candy
\rightarrow
retry
}
$$

という最小循環が成立します。

このメモがあれば、いったんこのスレッドを cw-tools / KK の観察へ戻しても、sui の議論はかなり正確に再開できます。

i.e. 文法は、反応の連鎖の中で形成される踏み跡である。

    『森の中に形成される踏み跡（tracks in a forest）』日本語の『獣道』
    『道具化』 Toolification

Nielsen and Christiansen (2026), Evidence for the Representation of Non-Hierarchical Structures in Language, Nature Human Behaviour
