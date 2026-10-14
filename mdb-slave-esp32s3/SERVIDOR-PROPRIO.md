# Fork: operar com servidor próprio (sem a plataforma VMflow)

Este fork mantém o firmware original (MDB, BLE, DEX, OTA, PAX) e muda o que é
preciso pra um servidor seu aprovar cada venda:

| Mudança | Onde |
|---|---|
| Broker, usuário, senha e prefixos de tópico no menuconfig; `wss://` com verificação de certificado | menu **MQTT Server** |
| Moeda padrão BRL | menu **MDB Cashless Device** |
| Conversão de preço com conta inteira (o original perdia 1 centavo em ~6% dos valores) | `mdb_to_cents()` / `cents_to_mdb()` |
| Sessão sempre aberta com saldo desconhecido (0xFFFF), como o comando BLE 0x02 | `CONFIG_ALWAYS_OPEN_SESSION` |
| Fila persistente em NVS pros eventos de venda; só sai com PUBACK | `event-queue.c`, `CONFIG_EVENT_QUEUE_MAX` |
| Publica `vend_request` e `vend_success` | MQTT |
| Comandos `approve:<centavos>` e `deny` | RPC |
| Nega sozinho se ninguém aprovar | `CONFIG_VEND_APPROVAL_TIMEOUT_SEC` |
| Sem venda cashless antes do relógio acertar; venda em dinheiro carimbada depois do SNTP | `clock_is_synced()`, `event_queue_push_unstamped()` |
| OTA desligado ou apontando pro seu repositório | menu **OTA** |

## Fluxo de uma venda

```
máquina liga o leitor ─► placa abre sessão com saldo desconhecido (0xFFFF)
cliente aperta a seleção ─► VMC: VEND_REQUEST ─► placa publica vend_request
servidor cobra (ex.: Point Smart) ─► RPC approve:<centavos> ─► placa: VEND APPROVED no próximo POLL
máquina libera ─► VEND_SUCCESS ─► placa publica vend_success
                  (ou VEND_FAILURE ─► vend_fail)
SESSION_COMPLETE ─► placa reabre a sessão com saldo desconhecido
```

Recusa (`deny`), cancelamento da máquina ou nenhum approve em
`CONFIG_VEND_APPROVAL_TIMEOUT_SEC` → VEND DENIED, e a sessão continua aberta.

## Mensagens

Assinadas: `<campos>:<ts>:<hmac>`, hmac = HMAC-SHA256(passkey, tudo antes do último `:`), hex minúsculo.

| Tópico (saída) | Payload |
|---|---|
| `<prefixo>/<sub>/vend_request` | `<centavos>:<selecao>:<ts>:<hmac>` |
| `<prefixo>/<sub>/vend_success` | `<centavos>:<selecao>:<ts>:<hmac>` |
| `<prefixo>/<sub>/vend_fail` | `<preco_mdb>,<selecao>:<ts>:<hmac>` (preço na escala MDB, como no original) |
| `<prefixo>/<sub>/vend_cancel` | `<centavos>:<selecao>:<ts>:<hmac>` (o VMC mandou VEND CANCEL com o pedido ainda esperando o servidor) |
| `<prefixo>/<sub>/sale` | `<centavos>:<selecao>:<ts>:<hmac>` (venda em dinheiro) |
| `<prefixo>/<sub>/status` | `online,<reset_reason>` / `offline` (last will), retidos, **não** assinados |
| `<prefixo>/<sub>/rpc/confirm` | `ok` / `fail`, **não** assinado |
| `<prefixo>/<sub>/rpc/info` | JSON; agora com `events_pending`, `events_dropped`, `clock_synced` e `time` |

Comandos (tópico `<prefixo_rpc>/<sub>/rpc`, `<cmd>:<args>:<ts>:<hmac>`, ts com no
máximo 10 s de diferença do relógio da placa):

| Comando | Efeito |
|---|---|
| `approve:<centavos>` | aprova o pedido pendente se o valor for exatamente o preço pedido; `ok`/`fail` em rpc/confirm |
| `deny:-` | nega o pedido pendente; `ok`/`fail` em rpc/confirm |
| `dex:-`, `info:-`, `echo:-`, `buzzer:-`, `oos:-`, `restart:-`, `credit:<c>` | como no original (`credit` não tem efeito com a sessão sempre aberta) |
| `ota:-` / `ota:<tag>` | só se `CONFIG_OTA_GITHUB_REPO` estiver preenchido |

Padrões: `<prefixo>` = `maquinas`, `<prefixo_rpc>` = `cmd`, usuário MQTT = subdomain
da placa. No servidor (`bot_vending`): `MQTT_TOPICO_SAIDA=maquinas/{sub}`,
`MQTT_TOPICO_RPC=cmd/{sub}/rpc`, e no broker `MQTT_PREFIXO_SAIDA=maquinas`.

## Relógio (SNTP)

Depois de ligar, o relógio da placa marca 1970 até o SNTP sincronizar. Um `ts` de
1970 é rejeitado pelo servidor (`ts_fora_janela`), e o `approve` também depende do
relógio. Por isso:

- A **sessão sempre aberta só abre depois do sync**. Se a máquina habilitar o leitor
  antes, a sessão abre no momento do sync. Enquanto isso, um VEND_REQUEST com saldo
  desconhecido é negado na hora.
- **Venda em dinheiro antes do sync** vai pra fila sem `ts` e sem assinatura, junto com
  o uptime da placa. Depois do sync, a fila calcula a hora real da venda
  (`agora − (uptime agora − uptime da venda)`), assina, **regrava a linha na NVS** e só
  então publica. Os reenvios saem idênticos e o servidor deduplica.
- Se a placa **reiniciar antes do sync**, o uptime antigo perde o sentido: a venda leva a
  hora do sync (o horário fica errado, mas a venda não se perde; a conferência do DEX
  é por contador, não por horário).
- O log mostra `clock synced by SNTP: <data> UTC`; o `rpc/info` traz `clock_synced` e
  `time`.

## Compilar e gravar

ESP-IDF **v5.5.1**.

```bash
cd mdb-slave-esp32s3
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p COM5 flash monitor
```

No `menuconfig`, em **VMflow**:

- **MQTT Server**: `Broker URI` (ex.: `wss://seu-broker.onrender.com:443/mqtt`), senha
  (o usuário vazio usa o subdomain), prefixos e tamanho da fila.
- **MDB Cashless Device**: endereço, moeda (BRL), fator de escala e casas decimais
  (iguais aos do servidor: `MDB_SCALE_FACTOR`/`MDB_DECIMAL_PLACES`), sessão sempre
  aberta e timeout de aprovação.
- **OTA**: deixe vazio ou ponha `seu-usuario/seu-fork`.

**Não faça commit do `sdkconfig` com a senha do broker.** Ele é versionado neste
repositório. Uma saída: preencher a senha só na sua cópia local e usar
`git update-index --skip-worktree mdb-slave-esp32s3/sdkconfig`.

Subdomain, passkey e Wi-Fi continuam sendo gravados por BLE pelo app Android do
projeto (a placa ainda anuncia `<sub>.vmflow.xyz`, que é o que o app procura). A
passkey tem no máximo 18 caracteres.

## Teste da fila no PC

```bash
sh test/host/run.sh
```

Compila o `main/event-queue.c` de verdade contra falsos de NVS/FreeRTOS/MQTT
(precisa de um compilador C: `cc`, `gcc`, `clang` ou `CC="python -m ziglang cc"`).

## Teste de bancada sem a máquina

Nenhum exemplo do repositório fazia o papel da máquina pro leitor cashless: o
`mdb-esp32s3-bridge` só é mestre de moedeiro (0x08) e noteiro (0x30). Por isso
existe `examples/mdb-esp32s3-vmc-sim`, que é o VMC (mestre do barramento) falando
com o cashless no endereço 0x10.

Material: duas ESP32-S3 DevKit, três jumpers, um broker MQTT acessível e o
servidor (ou o `simulador`/um cliente MQTT pra mandar approve/deny à mão).

Ligação, **sem a placa MDB no meio** (lógica 3,3 V direto):

| VMC simulado | Leitor (este firmware) |
|---|---|
| GPIO2 (TX) | GPIO4 (`PIN_MDB_RX`) |
| GPIO1 (RX) | GPIO5 (`PIN_MDB_TX`) |
| GND | GND |

Não ligue esses pinos no conector MDB da placa de produção nem na máquina: lá os
níveis são outros.

1. Grave este firmware numa DevKit, provisione subdomain/passkey/Wi-Fi pelo app e
   confira no monitor que ela conectou no broker (`MQTT_EVENT_CONNECTED`).
2. Grave o `mdb-esp32s3-vmc-sim` na outra:
   ```bash
   cd examples/mdb-esp32s3-vmc-sim
   idf.py set-target esp32s3
   idf.py -p COM6 flash monitor
   ```
   Deve aparecer `reader config: ... currency 0x1986 ...` e `BEGIN SESSION (funds
   unknown...)`.
3. No monitor do VMC simulado, digite os comandos (ou aperte BOOT = `v 12 350`):

| Teste | Comando no VMC simulado | Esperado |
|---|---|---|
| Pedido | `v 12 350` | broker recebe `vend_request` `350:12:...` |
| Aprovação | servidor manda `approve:350` | VMC: `VEND APPROVED`, depois `VEND SUCCESS`; broker recebe `rpc/confirm ok` e `vend_success` |
| Valor errado | servidor manda `approve:300` | `rpc/confirm fail`, nada aprovado |
| Recusa | `v 12 350`, servidor manda `deny` | VMC: `VEND DENIED`; sessão continua aberta |
| Falha | `m f`, depois `v 12 350` + approve | VMC manda `VEND FAILURE`; broker recebe `vend_fail` |
| Timeout | `m n`, `v 12 350`, sem approve | após `CONFIG_VEND_APPROVAL_TIMEOUT_SEC`: `VEND DENIED` |
| Cancelamento | `v 12 350`, depois `x` | `VEND DENIED`; broker recebe `vend_cancel` `350:12:...`; approve atrasado recebe `fail` |
| Dinheiro | `c 12 350` | broker recebe `sale` `350:12:...` |
| Sessão reabre | após qualquer venda | `END SESSION` seguido de `BEGIN SESSION (funds unknown...)` |
| Fila offline | desligue o Wi-Fi/broker, faça `c 12 350` duas vezes, reinicie o leitor, religue o broker | os dois `sale` chegam depois; `rpc/info` mostra `events_pending` voltando a 0 |
| Leitor desabilitado | `d`, depois `e` | sem sessão enquanto desabilitado; reabre no `e` |
| Venda antes do relógio | desligue o roteador/broker, tire o leitor da energia e ligue de novo (reset por software mantém a hora; sem energia ela volta a 1970 e, sem internet, não há SNTP), espere o VMC simulado habilitar, faça `c 12 350`, anote a hora, espere 2 min e religue a internet | log `clock synced by SNTP`; o `sale` chega com o `ts` da hora em que você fez a venda (não a do sync); `BEGIN SESSION (funds unknown)` só aparece depois do sync |
| Pedido antes do relógio | sem internet, depois de ligar a placa: `v 12 350` | `VEND DENIED` na hora, nenhum `vend_request` publicado |

O DEX usa outra porta (GPIO8/9) e não é coberto por esse simulador.

O que essa bancada **não** prova: tempo de resposta e níveis elétricos do
barramento real, o comportamento do VMC da TCN (timeouts, sessão com saldo
desconhecido, venda em dinheiro com sessão aberta) e o efeito das escritas em
flash no barramento. Isso fica pro teste na máquina, com o engenheiro.
