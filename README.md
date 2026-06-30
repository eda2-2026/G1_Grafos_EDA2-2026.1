# SeedForge

[Video sobre o projetro](https://drive.google.com/drive/u/1/folders/1v6iGKU_UUMfPOvAc9T9ZHKTVFEmkkLvs)

## 1. Minecraft

O **Minecraft** é um jogo onde o mundo é feito de cubos (blocos) de terra, pedra,
água, areia, madeira etc. Esse mundo é **gigantesco** e é gerado automaticamente
pelo computador conforme o jogador explora — ninguém desenha o mapa à mão.

O mundo é dividido em **biomas**: regiões com características próprias, como
*planície*, *deserto*, *floresta*, *montanha*, *oceano* ou *ilha de cogumelos*.
Cada bioma tem um clima, plantas e animais diferentes. Espalhadas pelo mapa também
há **estruturas** geradas pelo jogo: vilas, pirâmides do deserto, templos da selva,
mansões, cidades antigas etc. — muitas com baús contendo itens valiosos.

### O que é uma "seed" (semente)

Toda essa geração do mundo é **determinística**: ela parte de um único número
inteiro de 64 bits chamado **seed** (semente). A mesma seed gera **sempre o mesmo
mundo** — os mesmos biomas nos mesmos lugares, as mesmas estruturas, o mesmo loot
nos baús. Trocar a seed gera um mundo completamente diferente.

Isso significa que existem `2^64` mundos possíveis. Alguns jogadores procuram
seeds com propriedades raras e específicas, por exemplo:

- uma vila e uma pirâmide do deserto bem pertinho do ponto de nascimento;
- um diamante dentro de um baú de pirâmide;
- a **maior montanha contígua possível** (o tipo de busca que motiva este projeto).

O problema: testar uma seed exige simular parte da geração do mundo. Conferir
bilhões de seeds à mão é impossível. É aí que entra o SeedForge.

---

## 2. O que o SeedForge faz

O **SeedForge** é um programa que varre faixas enormes de seeds e mantém apenas as
que passam por um conjunto de **filtros** descritos num arquivo de configuração
(`.cfg`). Cada filtro é uma condição ("tem uma vila num raio de 500 blocos",
"o bioma neste ponto é deserto", "a maior montanha tem pelo menos X blocos") e
todas precisam ser verdadeiras ao mesmo tempo.

Para descobrir quais biomas e estruturas uma seed gera **sem abrir o jogo**, o
projeto usa a biblioteca [cubiomes](https://github.com/Cubitect/cubiomes), que
reproduz a matemática da geração de mundo do Minecraft. O SeedForge consulta essa
biblioteca, aplica os filtros e grava as seeds aprovadas num arquivo de saída.

Como são bilhões de seeds, o mesmo código de avaliação roda em dois alvos:

- **CPU** (`seedforge_cpu`) — versão de referência, paralelizada com OpenMP;
- **GPU** (`seedforge_gpu`) — versão CUDA, que testa milhares/milhões de seeds em paralelo.

> Os detalhes de configuração, filtros e formato do `.cfg` estão no **[GUIDE.md](GUIDE.md)**.

---

## 3. Grafos e Flood Fill

Esta é a parte central do trabalho de Estrutura de Dados.

### O problema: medir a MAIOR região contígua de um bioma

Queremos achar a seed com a **maior montanha** (ou maior floresta, oceano etc.).
"Maior" aqui não é "quantos blocos de montanha existem espalhados pelo mapa" — é o
**maior pedaço conectado**, onde dá para andar de uma ponta à outra sem sair do
bioma. Contar blocos soltos não serve: vários pedacinhos espalhados não formam uma
montanha grande de verdade. Precisamos medir **conectividade**, e conectividade é
um problema de **grafos**.

### Modelagem como grafo

A janela de busca ao redor de um ponto é amostrada como uma **grade** (grid) de
células, com espaçamento `step` blocos entre amostras. Modelamos essa grade como um
**grafo implícito**:

- **Vértice** = uma célula da grade que pertence ao bioma procurado;
- **Aresta** = ligação entre duas células **vizinhas** (acima, abaixo, esquerda,
  direita). Usamos vizinhança **4-conexa**.

Uma "região contígua de montanha" é, então, exatamente uma **componente conexa**
desse grafo. O tamanho da maior montanha é o tamanho da **maior componente conexa**.

```
. . X X . .          . = célula que NÃO é do bioma
. X X X . .          X = célula do bioma (vértice do grafo)
. X X . X .          arestas ligam X's vizinhos (horizontal/vertical)
. . . . X .
```

No exemplo acima existem duas componentes: um blob grande de 6 células e um par de
2 células. A maior tem 6.

### O algoritmo: Flood Fill (busca em componentes conexas)

Para medir a maior componente usamos **flood fill** — o mesmo algoritmo do "balde
de tinta" de editores de imagem, que é uma busca em grafo (DFS/BFS) restrita a uma
componente conexa. A implementação está em
[`src/eval.h`](src/eval.h) (caso `F_BIOME_SIZE`):

1. Percorremos todas as células da grade. Cada célula é marcada num **bitmap de
   visitados** (`seen`) para nunca ser processada duas vezes.
2. Ao encontrar uma célula do bioma ainda não visitada, iniciamos um flood fill a
   partir dela usando uma **pilha** (`stk`) — é uma DFS **iterativa** (sem
   recursão, para não estourar a pilha de chamadas em GPU).
3. Desempilhamos uma célula, contamos +1, e empilhamos seus **vizinhos 4-conexos**
   que sejam do mesmo bioma e ainda não tenham sido visitados.
4. Quando a pilha esvazia, terminamos uma componente conexa; guardamos o tamanho se
   ele for o maior visto até agora.
5. No fim, a maior componente vira a área da região: `área = células × step²`.

Estruturas de dados usadas (todas em memória contígua, sem alocação dinâmica para
poder rodar na GPU):

| Estrutura | Papel |
|-----------|-------|
| **Pilha** (`stk`) | fronteira da DFS — células descobertas e ainda não expandidas |
| **Bitmap de visitados** (`seen`) | marca cada vértice já alcançado em 1 bit; evita reprocessar e ciclos |
| **Grade implícita** | os vértices não são guardados numa lista; o índice da célula codifica `(x, z)` |

### Por que iterativo e com vizinhança implícita

O grafo **não é armazenado** como lista de adjacência. Os vizinhos de uma célula
de índice `c` numa grade de lado `side` são calculados na hora: `c+1`, `c-1`
(esquerda/direita) e `c+side`, `c-side` (cima/baixo), respeitando as bordas. Isso
economiza memória — essencial na GPU, onde cada uma das milhares de threads precisa
do seu próprio espaço de trabalho. Por isso a janela é limitada a um número máximo
de células (`SF_FLOOD_MAX_CELLS`), e a busca é **iterativa com pilha explícita** em
vez de recursiva.

### Resumo da relação com a disciplina

- **Grafo**: grade de biomas como grafo 4-conexo implícito.
- **Componente conexa**: cada região contígua de um bioma.
- **Travessia (DFS/Flood Fill)**: mede o tamanho de cada componente.
- **Pilha + bitmap de visitados**: estruturas que sustentam a travessia em tempo
  `O(número de células)`, sem recursão e sem alocação dinâmica.

---

## 4. Como compilar e rodar

```bash
make lib     # compila a biblioteca cubiomes
make cpu     # compila o buscador em CPU (gcc + OpenMP)
make gpu     # compila o buscador em GPU (CUDA / nvcc)
```

```bash
./seedforge_cpu example.cfg     # versão CPU (portável, sem GPU)
./seedforge_gpu mountains.cfg   # versão GPU, busca de maior montanha
```

A saída é gravada no arquivo `out=` do `.cfg`, no formato
`seed <TAB> x <TAB> z <TAB> tamanho`, ordenada pelo **tamanho** (área medida pelo
flood fill) em ordem decrescente — então a **primeira linha é a melhor seed**.

Exemplos de configuração prontos: `example.cfg`, `mountains.cfg` (maior montanha,
o caso do flood fill), `village.cfg`, `cheese_cave.cfg`. A referência completa de
todos os filtros está em **[GUIDE.md](GUIDE.md)**.

---

## Créditos

Construído sobre [cubiomes](https://github.com/Cubitect/cubiomes) (MIT). Inspirado
no cubiomes-viewer e em buscadores de seed em CUDA. Este projeto é licenciado sob
GPL-3.0.
