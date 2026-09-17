# Katawa Shoujo — portage natif Nintendo Switch

Portage homebrew **natif** (`.nro`) du visual novel *Katawa Shoujo* (Four Leaf Studios, Ren'Py 6.16).
Ren'Py n'est pas utilisé sur la console : un moteur C++17 (SDL2 + OpenGL ES 2) exécute les scripts
d'origine.

Ce dépôt ne contient **aucun contenu du jeu** (images, sons, vidéos, textes). Les données sont générées
à partir de votre propre copie du jeu (version Steam).

## Fonctionnement

- **Dump de l'init** (`tools/dump_init.py`) : le vrai Ren'Py 6.16 du jeu est lancé sur PC jusqu'à la fin
  de son initialisation, puis le registre d'images, les transforms, les transitions, les personnages, les
  styles et les chaînes de toutes les langues sont sérialisés.
- **Convertisseur** (`tools/convert.py`) : les `.rpyc` sont linéarisés en bytecode (un fichier par
  langue) et chaque extrait Python est analysé par le parseur Python 2.7 embarqué dans le jeu
  (`tools/py2ast.py`).
- **Moteur** (`source/`) :
  - mini-interpréteur Python 2 (`py*.cpp`) ;
  - displayables, ATL et transitions à la Ren'Py (`disp`, `transform`, `transition`, `scene`) ;
  - texte riche (`text`) ;
  - mixeur audio multi-canal avec stb_vorbis (`audio`) ;
  - vidéos MPEG-1 lues avec pl_mpeg (`video`) ;
  - écrans de KS réécrits nativement (`screens`, `extras`).

## Compilation

Prérequis : devkitPro (devkitA64, libnx, `switch-sdl2`, `switch-sdl2_image`, `switch-sdl2_ttf`,
`switch-mesa`), Python 3, Git Bash, ffmpeg, et une copie Steam de Katawa Shoujo.

```sh
# 1. Extraire les archives du jeu et des langues
python tools/unrpa.py extracted "<jeu>/game/data.rpa"
for f in "<jeu>"/game/lang-*.rpa; do python tools/unrpa.py extracted_lang "$f"; done

# 2. Dumper l'init Ren'Py (nécessite une copie du jeu dans ../pc_ref avec game/zz_dump.rpy)
./tools/run_dump.sh

# 3. Convertir les scripts, copier les assets et transcoder les vidéos
python tools/convert.py
python tools/assets.py --videos

# 4. Compiler
./build_switch.sh      # -> KatawaShoujo.nro
./build_pc.sh          # build de test Windows -> build_pc/ks.exe
```

## Installation

Copier `KatawaShoujo.nro` dans `sdmc:/switch/`. Les sauvegardes et le journal sont écrits dans
`sdmc:/switch/KatawaShoujo/`. Le mode *title takeover* (maintenir R en lançant un jeu) est recommandé.

## Contrôles

| Bouton | Action |
|---|---|
| A / tactile | Avancer / valider |
| B / + | Menu de jeu / retour |
| X | Masquer la fenêtre |
| Y | Historique |
| L | Retour arrière |
| R | Saut on/off |
| ZR (maintenu) | Saut |
| ZL | Mode auto |

## Tests

Le fichier `test.txt` dans le dossier utilisateur (ou les variables d'environnement) permet des tests
automatiques : `KS_SHOTS=10,20` (captures d'écran), `KS_KEYS=12:A,15:S` (entrées simulées).

## Crédits

*Katawa Shoujo* © Four Leaf Studios. Bibliothèques : SDL2, stb_vorbis (domaine public), pl_mpeg (MIT).
