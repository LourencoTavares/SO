# PacmanIST — Projeto de Sistemas Operativos

Projeto desenvolvido no âmbito da UC de **Sistemas Operativos**, centrado na implementação do jogo **PacmanIST** em C, com evolução por fases desde uma versão local até uma arquitetura **cliente-servidor** com **named pipes**, **threads**, **sinais** e **sincronização concorrente**.

---

## Descrição

O **PacmanIST** é um jogo bidimensional em que o Pacman percorre um labirinto recolhendo pontos, evitando monstros e atravessando portais para avançar de nível.

Ao longo do projeto foram trabalhados vários conceitos de Sistemas Operativos, incluindo:

- interação com o sistema de ficheiros através de **POSIX file descriptors**
- criação de processos com **fork()**
- paralelização com **threads**
- sincronização entre tarefas
- comunicação entre processos com **FIFOs**
- tratamento de sinais
- arquitetura **servidor-cliente**

---

## Entregas do Projeto

### 1ª Entrega — PacmanIST local

A primeira parte do projeto consistiu em três componentes principais:

#### 1. Interação com o sistema de ficheiros
- leitura de níveis a partir de ficheiros `.lvl`
- leitura do comportamento dos monstros em ficheiros `.m`
- leitura do comportamento do Pacman em ficheiros `.p`, quando aplicável
- carregamento automático de vários níveis a partir de uma diretoria

#### 2. Reencarnação do Pacman
- possibilidade de guardar um estado do jogo
- recuperação desse estado após a morte do Pacman
- utilização de `fork()` para implementar a lógica de recuperação

#### 3. Paralelização com múltiplas tarefas
- cada personagem passou a ser gerida por uma **thread**
- separação entre:
  - threads que atualizam o estado do jogo
  - thread responsável pela interação com `ncurses`
- sincronização do estado partilhado do tabuleiro

---

### 2ª Entrega — Servidor PacmanIST e clientes remotos

A segunda parte do projeto estendeu o PacmanIST para uma arquitetura distribuída.

#### Funcionalidades principais
- transformação do PacmanIST num **servidor autónomo**
- suporte a múltiplos clientes ligados remotamente
- comunicação entre servidor e clientes com **named pipes**
- envio periódico de atualizações do tabuleiro
- leitura de comandos do cliente e envio para o servidor
- suporte a múltiplas sessões de jogo em paralelo
- utilização de **buffer produtor-consumidor** com **semáforos** e **mutexes**

#### Sinais e logging
- redefinição do tratamento do sinal `SIGUSR1`
- geração de ficheiro com os **5 clientes ligados com maior pontuação**
- bloqueio de sinais nas threads que não deveriam receber `SIGUSR1`

#### Nota sobre a base usada na 2ª entrega
Como a segunda entrega dependia diretamente da primeira, foi usada como base a **solução completa fornecida para a 2ª parte**, que já incluía uma possível implementação da 1ª parte.  
Isto permitiu garantir compatibilidade com a arquitetura cliente-servidor e evitar que eventuais problemas da 1ª entrega comprometessem o resto da implementação.

---

## Funcionalidades

- leitura de níveis e comportamentos a partir de ficheiros
- execução do jogo em terminal com `ncurses`
- múltiplas entidades concorrentes no tabuleiro
- persistência temporária de estado com `fork()`
- servidor de jogo com múltiplos clientes
- named pipes para comunicação IPC
- protocolo de mensagens entre cliente e servidor
- signal handling com geração de ficheiro de top clientes

---

## Estrutura do Projeto

### 1ª entrega
- `src/` -> código fonte do jogo
- `include/` -> ficheiros de cabeçalho
- `test_levels/` -> níveis e comportamentos de teste
- `Makefile` -> compilação do projeto

### 2ª entrega
#### Servidor
- `SO-2526-sol-parte1/`
  - `src/`
  - `include/`
  - `levels/`
  - `Makefile`

#### Cliente
- `client-base-with-Makefile-v3/`
  - `src/client/`
  - `include/`
  - `moves/`
  - `Makefile`

---

## Como executar

- No 1º terminal (Servidor), escrever os seguintes comandos:

``
cd ../SO-2526-sol-parte1
./bin/Pacmanist levels 1 /tmp/pacman_reg
``

- No 2º terminal (Cliente), escrever os seguintes comandos:

``
cd ../client-base-with-Makefile-v3
./bin/client /tmp/client_req /tmp/client_notif /tmp/pacman_reg
``

- Para executar o cliente com movimentos automáticos:

``
./bin/client /tmp/client_req /tmp/client_notif /tmp/pacman_reg < moves/moves1.txt
``


