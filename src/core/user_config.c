/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/user_config.h"

#include "pal/pal.h"

const user_control user_controls[] = {
    {"cima", "Direcional: cima", PAL_PAD_UP, "UP"},
    {"baixo", "Direcional: baixo", PAL_PAD_DOWN, "DOWN"},
    {"esquerda", "Direcional: esquerda", PAL_PAD_LEFT, "LEFT"},
    {"direita", "Direcional: direita", PAL_PAD_RIGHT, "RIGHT"},
    {"cruz", "X (cruz)", PAL_PAD_CROSS, "SPACE,Z"},
    {"circulo", "O (círculo)", PAL_PAD_CIRCLE, "X,BACKSPACE"},
    {"quadrado", "Quadrado", PAL_PAD_SQUARE, "C"},
    {"triangulo", "Triângulo", PAL_PAD_TRIANGLE, "V"},
    {"l1", "L1", PAL_PAD_L1, "Q"},
    {"r1", "R1", PAL_PAD_R1, "E"},
    {"l2", "L2", PAL_PAD_L2, "1"},
    {"r2", "R2", PAL_PAD_R2, "3"},
    {"l3", "L3", PAL_PAD_L3, "F"},
    {"r3", "R3", PAL_PAD_R3, "G"},
    {"options", "Options", PAL_PAD_OPTIONS, "ENTER"},
    {"touchpad", "Touchpad", PAL_PAD_TOUCH_PAD, "T"},
    {"touchpad_esq", "Touchpad: esquerda", PAL_PAD_TOUCH_LEFT, "R"},
    {"esq_cima", "Analógico esquerdo: cima", PAL_STICK_LEFT_UP, "W"},
    {"esq_baixo", "Analógico esquerdo: baixo", PAL_STICK_LEFT_DOWN, "S"},
    {"esq_esquerda", "Analógico esquerdo: esquerda", PAL_STICK_LEFT_LEFT, "A"},
    {"esq_direita", "Analógico esquerdo: direita", PAL_STICK_LEFT_RIGHT, "D"},
    {"dir_cima", "Analógico direito: cima", PAL_STICK_RIGHT_UP, "I"},
    {"dir_baixo", "Analógico direito: baixo", PAL_STICK_RIGHT_DOWN, "K"},
    {"dir_esquerda", "Analógico direito: esquerda", PAL_STICK_RIGHT_LEFT, "J"},
    {"dir_direita", "Analógico direito: direita", PAL_STICK_RIGHT_RIGHT, "L"},
};
const uint32_t user_control_count = PS5C_ARRAY_COUNT(user_controls);

const char user_config_default_text[] =
    "; ZeraPS5 - configurações do usuário\n"
    "; Fica na pasta do emulador: vai junto quando a pasta é copiada.\n"
    "; Pode ser editado à mão ou pelo launcher (ZeraPS5.exe).\n"
    "; Configurações de um jogo só: user\\config\\<ID do jogo>.ini, com as mesmas\n"
    "; seções e apenas o que muda para aquele jogo.\n"
    "\n"
    "[interface]\n"
    "; idioma do launcher, independente do console/jogo\n"
    "idioma=pt-BR\n"
    "\n"
    "[atualizacao]\n"
    "ao_iniciar=1\n"
    "\n"
    "[geral]\n"
    "; idioma do console: 17 Português (Brasil), 1 English (US), 18 English (UK),\n"
    "; 3 Español, 2 Français, 4 Deutsch, 5 Italiano, 7 Português (Portugal), 0 Japonês\n"
    "idioma=17\n"
    "; resolução da TV que o jogo vê: 1080 ou 2160 (4K)\n"
    "resolucao=1080\n"
    "; console que o jogo vê: 0 PS5, 1 PS5 Pro (experimental; só para jogos com suporte ao Pro)\n"
    "console=0\n"
    "\n"
    "[pastas]\n"
    "; pastas com jogos (jogos1, jogos2, ...): relativas à pasta do emulador ou completas (D:\\Jogos PS5)\n"
    "jogos1=games\n"
    "; bibliotecas de sistema do seu console\n"
    "sistema=system\\modules\n"
    "\n"
    "[video]\n"
    "; placa de vídeo: auto (a dedicada, se houver), o nome dela como o launcher mostra, ou o número\n"
    "placa=auto\n"
    "\n"
    "[entrada]\n"
    "; Entrada nativa para jogos compativeis; teclado exige controle conectado.\n"
    "teclado_nativo=0\n"
    "; Clique no jogo captura o mouse. F8 solta/captura; Alt+Tab solta.\n"
    "mouse_nativo=0\n"
    "\n"
    "[teclado]\n"
    "; teclas de cada controle, separadas por vírgula. Nomes: A-Z, 0-9, F1-F12, UP, DOWN,\n"
    "; LEFT, RIGHT, SPACE, ENTER, BACKSPACE, TAB, ESC, SHIFT, CTRL, ALT, INSERT, DELETE,\n"
    "; HOME, END, PAGEUP, PAGEDOWN, NUM0-NUM9. DualSense, DualShock 4 e Xbox funcionam sempre.\n"
    "cima=UP\n"
    "baixo=DOWN\n"
    "esquerda=LEFT\n"
    "direita=RIGHT\n"
    "cruz=SPACE,Z\n"
    "circulo=X,BACKSPACE\n"
    "quadrado=C\n"
    "triangulo=V\n"
    "l1=Q\n"
    "r1=E\n"
    "l2=1\n"
    "r2=3\n"
    "l3=F\n"
    "r3=G\n"
    "options=ENTER\n"
    "; touchpad: clique na metade direita (START nos clássicos de PSP/PS1);\n"
    "; touchpad_esq: na metade esquerda (SELECT)\n"
    "touchpad=T\n"
    "touchpad_esq=R\n"
    "esq_cima=W\n"
    "esq_baixo=S\n"
    "esq_esquerda=A\n"
    "esq_direita=D\n"
    "dir_cima=I\n"
    "dir_baixo=K\n"
    "dir_esquerda=J\n"
    "dir_direita=L\n"
    "\n"
    "[controle]\n"
    "; DualSense/DualShock 4: vibração nos jogos (1 liga, 0 desliga)\n"
    "vibracao=1\n";
