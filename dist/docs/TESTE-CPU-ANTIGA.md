# Testar em Intel Core de 3ª geração

Esta integração da versão 0.1.1 inclui o trabalho do Claude para substituir
instruções de CPU do jogo que o Ivy Bridge não possui. Também preserva as
configurações de resolução interna e o painel F10 do main.

1. Extraia o pacote inteiro em uma pasta nova na máquina de teste.
2. Abra `ZeraPS5.exe` e adicione a pasta dos seus jogos nas configurações.
3. Em **Processador antigo**, deixe **Automático** e salve. O programa detecta
   as instruções disponíveis. **Sempre ligado (teste)** simula um Ivy Bridge
   em um processador moderno; **Desligado** permite comparar sem substituições.
4. Use os mesmos arquivos de sistema e jogos próprios utilizados no teste
   anterior. O pacote não inclui bibliotecas do console, jogos nem saves.
5. Pressione **F10** durante o jogo para conferir CPU, SO, RAM, GPU e FPS.
   Registre o jogo, o trecho e o build ao comparar o resultado.

A opção **Sem som** é destinada a computadores sem saída de áudio. Ela não
é necessária para ativar a compatibilidade da CPU. Se o dispositivo de áudio
parar de consumir amostras, o jogo pode continuar usando o relógio emulado.

O alvo deste modo é uma CPU x64 com AVX, como o Ivy Bridge. A compatibilidade
da CPU não substitui os requisitos Vulkan da placa de vídeo. Os testes locais
usaram Ryzen 7 5700G e RTX 5060 Ti; o teste físico no i5/Windows 10 é separado.
