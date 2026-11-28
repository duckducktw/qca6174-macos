> Historical archive only. The companion application described below was removed; current builds and releases contain the native kext only. See the root README.

# Historical legacy-driver background

These upstream notes describe the Ethernet-backed driver, not the current native diagnostic adapter.

# Ath10kMac

[English](#english) | [Português (Brasil)](#português-brasil)

## English

### Overview

Ath10kMac is an experimental macOS kernel extension for Qualcomm Atheros Wi-Fi adapters on Intel x86_64 Macs and Hackintosh systems. It is a community fork of [jonasw8/Ath10kMac](https://github.com/jonasw8/Ath10kMac).

This fork documents and extends support for the QCA9377 found in one Dell Inspiron 15-3567. The driver is experimental. A successful build only means that the code compiled; it does not prove that Wi-Fi will work on a different adapter revision, laptop, macOS release, or router.

There is currently no prebuilt release package or installer. Build from source and test with a spare USB EFI before changing a working internal EFI.

### Work in this fork

- Added a QCA9377 board-data profile selected by the adapter's PCI IDs: vendor/device **168c:0042**, subsystem **1028:1810**. The firmware preparation step includes the corresponding board data in the driver payload.
- Extended the WPA2-Personal security path to handle AES/CCMP pairwise encryption with either AES/CCMP or TKIP group keys. This allows the tested mixed WPA2 configuration where the access point advertises both TKIP and CCMP pairwise suites and uses a TKIP group key. TKIP pairwise encryption is still unsupported.
- Added and expanded regression coverage for QCA9377 identification, WPA2 security-suite parsing, key installation/rekey handling, and security diagnostics.
- Fixed CE-ring DMA mapping to walk all `IODMACommand` segments and accept a ring only when they cover the full buffer contiguously within the adapter's 32-bit DMA address range. Added diagnostics for ring allocation failures. This addresses a 4 KiB segment result for a 64 KiB ring observed during recovery on the test system.
- Improved the Ath10k Connectivity companion app: menu-bar connection states, animated connecting indicator, optional Keychain password storage, preferred saved-network reconnect, Launch at Login, and a copyable access-point security report that does not include the Wi-Fi password.

### Hardware and software validation

The QCA9377 work in this fork was tested on:

| Item | Tested configuration |
| --- | --- |
| Laptop | Dell Inspiron 15-3567 |
| Wi-Fi adapter | Qualcomm Atheros QCA9377 |
| PCI vendor/device | 168c:0042 |
| PCI subsystem | 1028:1810 |
| Operating system | macOS Sequoia 15.6, build 24G84, x86_64 |
| Driver | Ath10kMac 0.0.98; loaded and CE-ring allocation succeeded in a follow-up run |
| Build tools | Xcode 16.4 |

On that configuration, network scanning, connection, and internet traffic were confirmed on the tested 2.4 GHz and 5 GHz networks. A WPA2-Personal mixed-cipher network using CCMP/AES pairwise and TKIP group encryption connected successfully.

A follow-up v0.0.98 run reported active power and successful CE-ring allocation, and Wi-Fi connected after a delay. Repeated startup and sleep/wake validation is still needed; the stage responsible for the delay has not been isolated.

These are results from one adapter, one laptop, and the networks tested. They are not a compatibility guarantee or a controlled throughput benchmark. The QCA6174 support and performance figures mentioned in the upstream project have not been independently revalidated as part of this QCA9377 work.

### How the driver and app work

Ath10kMac handles the adapter and Wi-Fi connection. macOS sees the resulting connection through an Ethernet-style network interface; the driver does not add the normal macOS Wi-Fi controls for this adapter.

Use **Ath10k Connectivity** to scan, choose a network, enter its password, connect, and view connection status. Keep the app running while connected. If enabled, the app stores the password in the macOS Keychain and tries saved networks in preferred order when it starts. Its menu-bar menu can be set to launch the app at login. Launch at Login uses macOS 13 or later.

After sleep, the app can reinitialize the adapter and reconnect. The Wi-Fi association is not maintained during sleep (WoWLAN is not implemented).

### Wi-Fi security support

| Network/security mode | Status |
| --- | --- |
| Open network | Supported |
| WPA2-Personal / PSK | Supported with AES/CCMP pairwise encryption and AES/CCMP or TKIP group encryption |
| Mixed WPA2 advertisement | Supported when the adapter can select CCMP pairwise; TKIP group encryption was tested |
| WPA3-Personal / SAE | Not supported |
| Required Protected Management Frames (PMF / 802.11w) | Not supported |
| TKIP pairwise encryption | Not supported |
| WPA/WPA2 Enterprise / EAP | Not supported |
| DFS channels | Not supported |
| Access point/host mode, AirDrop, and AWDL | Not supported |

Security support is intentionally limited. A router that requires WPA3, PMF, TKIP pairwise, or enterprise authentication will not connect. Optional PMF behavior has not been broadly tested.

### Known limitations and reported behavior

- QCA9377 support has been verified only on the PCI subsystem and laptop listed above. Other QCA9377 revisions need testing.
- Automatic reconnect after startup can be delayed on the tested system. It has sometimes stayed at “Reconnecting” or “Negotiating” before completing. Reconnection is best-effort; keep the app open and use Scan/Retry if it does not finish.
- Verbose/debug boot on the test system displayed repeated low-level Ath10kMac diagnostics and appeared to pause during startup. The boot-time impact and cause have not been measured or isolated. Reports should include whether verbose or Ath10k debug boot arguments were enabled.
- Sleep recovery restarts the adapter and reconnects; it does not preserve the previous Wi-Fi session. The v0.0.98 DMA-ring path succeeded in one follow-up run, but repeated wake tests are still needed.
- The kext is experimental and can cause a kernel panic or prevent startup if a device or configuration is incompatible.

### Build from source

Build on an Intel Mac with macOS, Xcode and its command-line tools, Python 3, and the repository's MacKernelSDK submodule.

Clone the repository and initialize its submodules:

```sh
git clone --recurse-submodules https://github.com/thekunalthakur/Ath10kMac.git
cd Ath10kMac
```

If you already cloned without submodules, run:

```sh
git submodule update --init --recursive
```

Build the kext and the companion app:

```sh
make clean
make kext
make app
```

The firmware preparation and kext symbol check run as part of the build. Outputs:

- Kext: `build/out/Ath10kMac.kext`
- App: `build/Ath10k Connectivity.app`

The project build targets Intel x86_64. QCA9377 runtime validation documented above was on Sequoia 15.6. Building successfully does not validate the firmware, OpenCore setup, or Wi-Fi behavior on another system.

### Test with OpenCore

**Do not replace a working internal EFI for the first test.** Keep a recoverable backup and boot from a separate USB EFI first.

1. Build the kext and copy `build/out/Ath10kMac.kext` into the USB EFI at `EFI/OC/Kexts/`.
2. In OpenCore's `Kernel -> Add`, add an entry for `Ath10kMac.kext` with:
   - Arch: `x86_64`
   - BundlePath: `Ath10kMac.kext`
   - ExecutablePath: `Contents/MacOS/Ath10kMac`
   - PlistPath: `Contents/Info.plist`
   - Enabled: `True`
3. Validate `config.plist` with the tools matching your OpenCore version and boot using the USB's OpenCore entry.
4. Confirm that the kext loads, scan and connect with Ath10k Connectivity, and verify actual traffic before considering any internal-EFI change.

OpenCore configurations differ by system. Preserve the existing kext order and other required kexts; this README does not provide a universal EFI or config.plist.

### Report a test or issue

Reports from other machines—especially QCA9377 systems—are welcome. Include:

- Laptop and adapter model, adapter revision if known, PCI vendor/device and subsystem IDs.
- macOS version/build, Ath10kMac version, and Ath10k Connectivity version.
- Wi-Fi band, channel, and security mode (for example, WPA2-PSK with CCMP pairwise and TKIP group).
- What happened, what you expected, and steps to reproduce it.
- Whether the problem occurs at boot, scan, authentication/negotiation, traffic, disconnect, or wake.
- For boot/reconnect delays, whether verbose or debug boot arguments were enabled.

To collect diagnostics from the project directory:

```sh
sudo sh scripts/test-runtime.sh > ~/Desktop/ath10k-runtime.txt
```

The script samples Ath10kMac runtime state, reads recent kernel logs, and records network-interface/DHCP details. It writes the report to the Desktop; it does not upload it. Review and redact SSIDs, BSSIDs, MAC addresses, IP addresses, serial numbers, and any other private details before attaching it. Do not include Wi-Fi passwords or an unredacted EFI/config.plist.

### License

The Ath10kMac project code in this repository is released under the **GNU General Public License v3.0 (GPL-3.0)**. See the repository-root [LICENSE](LICENSE). Third-party files and bundled assets can have separate terms.

The QCA9377 firmware is distributed under separate Qualcomm Atheros terms and includes additional notices. Review [the firmware license](firmware/LICENSE.QualcommAtheros_ath10k), [the firmware notices](firmware/QCA9377/hw1.0/notice_ath10k_firmware-6.txt), and [the provenance record](firmware/QCA9377/hw1.0/PROVENANCE.json) before redistributing firmware. The prepared firmware payload is derived from those files; the root GPL license does not replace their terms.

MacKernelSDK is included as a Git submodule and retains the license provided by its upstream project. See [Acidanthera/MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) for its terms.

### Credits

This fork builds on [jonasw8/Ath10kMac](https://github.com/jonasw8/Ath10kMac) and the wider community work already credited in the project. See the source files and firmware directory for component licenses, provenance, and detailed acknowledgments.

## Português (Brasil)

### Visão geral

Ath10kMac é uma extensão de kernel experimental para macOS, destinada a adaptadores Qualcomm Atheros em Macs e Hackintosh Intel x86_64. Este repositório é um fork comunitário de [jonasw8/Ath10kMac](https://github.com/jonasw8/Ath10kMac).

Este fork documenta e amplia o suporte à QCA9377 encontrada em um Dell Inspiron 15-3567. O driver continua experimental. Compilar com sucesso só confirma que o código foi compilado; não garante Wi-Fi em outra revisão da placa, computador, versão do macOS ou roteador.

No momento não há pacote de instalação nem versão pré-compilada publicada. Compile o código-fonte e teste primeiro com uma EFI USB separada, sem alterar a EFI interna que já funciona.

### Trabalho neste fork

- Adicionado um perfil de dados de placa para QCA9377, selecionado pelos IDs PCI: vendor/device **168c:0042** e subsystem **1028:1810**. A preparação do firmware inclui os dados correspondentes no payload do driver.
- Ampliado o caminho de segurança WPA2-Personal para aceitar criptografia pairwise AES/CCMP com chave de grupo AES/CCMP ou TKIP. Isso permite a configuração WPA2 mista testada, na qual o ponto de acesso anuncia TKIP e CCMP como opções pairwise e usa TKIP para a chave de grupo. TKIP pairwise continua sem suporte.
- Ampliada a cobertura de testes de regressão para identificação da QCA9377, interpretação das suites WPA2, instalação/renovação de chaves e diagnósticos de segurança.
- Corrigido o mapeamento DMA dos anéis CE para percorrer todos os segmentos `IODMACommand` e aceitar um anel somente quando eles cobrem o buffer inteiro de forma contígua dentro do limite de endereçamento DMA de 32 bits do adaptador. Foram adicionados diagnósticos de falha de alocação. Isso trata um resultado de segmento de 4 KiB para um anel de 64 KiB observado durante a recuperação no sistema de teste.
- Melhorado o aplicativo Ath10k Connectivity: estados de conexão na barra de menu, indicador animado durante a conexão, armazenamento opcional da senha no Keychain, reconexão na ordem preferida das redes salvas, opção Launch at Login e relatório copiável da segurança anunciada pelo ponto de acesso, sem a senha do Wi-Fi.

### Hardware e software testados

O trabalho de QCA9377 deste fork foi testado em:

| Item | Configuração testada |
| --- | --- |
| Notebook | Dell Inspiron 15-3567 |
| Adaptador Wi-Fi | Qualcomm Atheros QCA9377 |
| PCI vendor/device | 168c:0042 |
| PCI subsystem | 1028:1810 |
| Sistema operacional | macOS Sequoia 15.6, build 24G84, x86_64 |
| Driver | Ath10kMac 0.0.98; carregado e alocação dos anéis CE confirmada em um teste posterior |
| Ferramentas de compilação | Xcode 16.4 |

Nessa configuração, foram confirmados a busca de redes, a conexão e o tráfego de internet nas redes testadas de 2,4 GHz e 5 GHz. Uma rede WPA2-Personal mista, com CCMP/AES pairwise e TKIP para a chave de grupo, conectou com sucesso.

Em um teste posterior com v0.0.98, o sistema informou energia ativa e alocação bem-sucedida dos anéis CE, e o Wi-Fi conectou após um atraso. Ainda são necessários testes repetidos de inicialização e repouso/retomada; a etapa responsável pelo atraso não foi isolada.

Esses resultados valem para um adaptador, um notebook e as redes testadas. Não são uma garantia de compatibilidade nem um benchmark controlado de velocidade. O suporte e os números de desempenho da QCA6174 mencionados no projeto upstream não foram revalidados independentemente neste trabalho com QCA9377.

### Como o driver e o aplicativo funcionam

A Ath10kMac controla o adaptador e a conexão Wi-Fi. O macOS vê a conexão por meio de uma interface de rede semelhante a Ethernet; o driver não adiciona os controles normais de Wi-Fi do macOS para esse adaptador.

Use o **Ath10k Connectivity** para buscar redes, selecionar uma rede, informar a senha, conectar e acompanhar o estado da conexão. Mantenha o aplicativo aberto enquanto estiver conectado. Se habilitada, a opção salva a senha no Keychain do macOS e tenta reconectar às redes salvas na ordem preferida quando o aplicativo inicia. O menu da barra pode iniciar o aplicativo no login. A opção Launch at Login exige macOS 13 ou posterior.

Depois do repouso, o aplicativo pode reinicializar o adaptador e reconectar. A associação Wi-Fi não é mantida durante o repouso (WoWLAN não está implementado).

### Segurança Wi-Fi compatível

| Modo de rede/segurança | Situação |
| --- | --- |
| Rede aberta | Compatível |
| WPA2-Personal / PSK | Compatível com criptografia pairwise AES/CCMP e criptografia de grupo AES/CCMP ou TKIP |
| Anúncio WPA2 misto | Compatível quando o adaptador pode escolher CCMP pairwise; TKIP para chave de grupo foi testado |
| WPA3-Personal / SAE | Sem suporte |
| Protected Management Frames (PMF / 802.11w) obrigatório | Sem suporte |
| Criptografia pairwise TKIP | Sem suporte |
| WPA/WPA2 Enterprise / EAP | Sem suporte |
| Canais DFS | Sem suporte |
| Modo ponto de acesso, AirDrop e AWDL | Sem suporte |

O suporte de segurança é limitado intencionalmente. Um roteador que exija WPA3, PMF, TKIP pairwise ou autenticação Enterprise não conectará. O comportamento com PMF opcional ainda não foi amplamente testado.

### Limitações e comportamentos observados

- O suporte QCA9377 só foi verificado no notebook e subsystem PCI listados acima. Outras revisões da QCA9377 precisam ser testadas.
- A reconexão automática após iniciar o sistema pode demorar no equipamento testado. Às vezes permaneceu em “Reconnecting” ou “Negotiating” antes de concluir. A reconexão é best-effort; mantenha o aplicativo aberto e use Scan/Retry se não terminar.
- Com boot verbose/debug no equipamento de teste, apareceram mensagens repetidas de diagnóstico de baixo nível da Ath10kMac e o início pareceu pausado. O impacto e a causa do atraso não foram medidos nem isolados. Relatos devem informar se os argumentos verbose ou Ath10k debug estavam ativos.
- Ao voltar do repouso, o adaptador é reinicializado e reconectado; a sessão Wi-Fi anterior não é preservada. O caminho de anéis DMA do v0.0.98 funcionou em um teste posterior, mas ainda são necessários testes repetidos de retomada.
- A kext é experimental e pode causar kernel panic ou impedir o início do sistema se o dispositivo ou a configuração forem incompatíveis.

### Compilar do código-fonte

Compile em um Mac Intel com macOS, Xcode e ferramentas de linha de comando, Python 3 e o submódulo MacKernelSDK do repositório.

Clone o repositório e inicialize os submódulos:

```sh
git clone --recurse-submodules https://github.com/thekunalthakur/Ath10kMac.git
cd Ath10kMac
```

Se você já clonou sem os submódulos, execute:

```sh
git submodule update --init --recursive
```

Compile a kext e o aplicativo:

```sh
make clean
make kext
make app
```

A preparação do firmware e a verificação de símbolos da kext ocorrem durante a compilação. Arquivos gerados:

- Kext: `build/out/Ath10kMac.kext`
- Aplicativo: `build/Ath10k Connectivity.app`

A compilação é direcionada a Intel x86_64. A validação de QCA9377 descrita acima foi feita no Sequoia 15.6. Compilar com sucesso não valida o firmware, a configuração OpenCore ou o funcionamento do Wi-Fi em outro sistema.

### Testar com OpenCore

**Não substitua uma EFI interna funcional no primeiro teste.** Mantenha um backup recuperável e inicialize primeiro por uma EFI USB separada.

1. Compile a kext e copie `build/out/Ath10kMac.kext` para `EFI/OC/Kexts/` na EFI USB.
2. Em `Kernel -> Add` do OpenCore, crie uma entrada para `Ath10kMac.kext` com:
   - Arch: `x86_64`
   - BundlePath: `Ath10kMac.kext`
   - ExecutablePath: `Contents/MacOS/Ath10kMac`
   - PlistPath: `Contents/Info.plist`
   - Enabled: `True`
3. Valide o `config.plist` com ferramentas compatíveis com sua versão do OpenCore e inicialize pela entrada OpenCore da USB.
4. Confirme o carregamento da kext, busque uma rede, conecte pelo Ath10k Connectivity e verifique o tráfego antes de considerar qualquer mudança na EFI interna.

As configurações do OpenCore variam entre computadores. Preserve a ordem das kexts existentes e outras kexts necessárias; este README não fornece uma EFI ou config.plist universal.

### Relatar testes e problemas

Relatos de outros computadores — especialmente com QCA9377 — são bem-vindos. Inclua:

- Modelo do notebook e adaptador; revisão da placa, se conhecida; IDs PCI vendor/device e subsystem.
- Versão/build do macOS, da Ath10kMac e do Ath10k Connectivity.
- Banda Wi-Fi, canal e segurança da rede (por exemplo, WPA2-PSK com CCMP pairwise e TKIP para a chave de grupo).
- O que aconteceu, o que era esperado e os passos para reproduzir.
- Se o problema ocorre ao iniciar, buscar redes, autenticar/negociar, transferir dados, desconectar ou voltar do repouso.
- Para atrasos no início ou reconexão, informe se verbose ou debug estavam ativos.

Para coletar diagnósticos na pasta do projeto:

```sh
sudo sh scripts/test-runtime.sh > ~/Desktop/ath10k-runtime.txt
```

O script coleta amostras do estado da Ath10kMac, lê logs recentes do kernel e registra detalhes de interfaces de rede/DHCP. Ele grava o relatório na pasta Desktop; não faz upload. Revise e remova SSIDs, BSSIDs, endereços MAC, endereços IP, números de série e outros dados privados antes de anexar. Não inclua senhas de Wi-Fi nem uma EFI/config.plist sem redigir os dados.

### Licença

O código do projeto Ath10kMac neste repositório é distribuído sob a **GNU General Public License versão 3.0 (GPL-3.0)**. Consulte o [LICENSE](LICENSE) na raiz do repositório. Arquivos de terceiros e recursos incluídos podem ter termos próprios.

O firmware QCA9377 é distribuído sob termos separados da Qualcomm Atheros e inclui avisos adicionais. Antes de redistribuir firmware, consulte [a licença do firmware](firmware/LICENSE.QualcommAtheros_ath10k), [os avisos do firmware](firmware/QCA9377/hw1.0/notice_ath10k_firmware-6.txt) e [o registro de proveniência](firmware/QCA9377/hw1.0/PROVENANCE.json). O payload de firmware preparado é derivado desses arquivos; a GPL na raiz não substitui os termos deles.

O MacKernelSDK é incluído como submódulo Git e mantém a licença definida pelo projeto upstream. Consulte [Acidanthera/MacKernelSDK](https://github.com/acidanthera/MacKernelSDK).

### Créditos

Este fork se baseia em [jonasw8/Ath10kMac](https://github.com/jonasw8/Ath10kMac) e no trabalho da comunidade já reconhecido no projeto. Consulte os arquivos de código e a pasta firmware para licenças, proveniência e agradecimentos detalhados.

---
