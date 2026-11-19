#!/usr/bin/env python3
"""
PS5C exFAT Dump Extractor
Extrai imagens de partição de disco exFAT de jogos do PS5.
"""

import os
import sys
import argparse
from dissect.fat.exfat import ExFAT, RunlistStream

def extract_exfat(image_path: str, output_dir: str):
    if not os.path.exists(image_path):
        print(f"Erro: Arquivo '{image_path}' nao encontrado.", file=sys.stderr)
        sys.exit(1)

    os.makedirs(output_dir, exist_ok=True)
    print(f"Abrindo imagem: {image_path}")

    with open(image_path, "rb") as fh:
        fs = ExFAT(fh)
        volume_label = getattr(fs, "volume_label", "Sem Label")
        print(f"Volume Label: {volume_label}")
        print(f"Diretorio de saida: {output_dir}")
        print("-" * 60)

        total_files = 0
        total_bytes = 0

        def extract_tree(directory_dict, current_rel_dir=""):
            nonlocal total_files, total_bytes
            for name, item in directory_dict.items():
                if name in (".", "..", "/"):
                    continue

                entry, children = item
                is_dir = bool(entry.metadata.attributes.directory)
                rel_path = os.path.join(current_rel_dir, name) if current_rel_dir else name
                target_path = os.path.join(output_dir, rel_path)

                if is_dir:
                    os.makedirs(target_path, exist_ok=True)
                    if children:
                        extract_tree(children, rel_path)
                else:
                    file_size = entry.stream.data_length
                    os.makedirs(os.path.dirname(target_path), exist_ok=True)
                    
                    if file_size > 0:
                        rl = fs.runlist(entry.stream.location, not_fragmented=entry.stream.flags.not_fragmented, size=file_size)
                        stream = RunlistStream(fs.filesystem, rl, file_size, fs.sector_size)
                        
                        with open(target_path, "wb") as out_f:
                            chunk_size = 1024 * 1024
                            remaining = file_size
                            while remaining > 0:
                                read_len = min(remaining, chunk_size)
                                chunk = stream.read(read_len)
                                if not chunk:
                                    break
                                out_f.write(chunk)
                                remaining -= len(chunk)
                    else:
                        with open(target_path, "wb") as out_f:
                            pass

                    total_files += 1
                    total_bytes += file_size
                    mb_size = file_size / (1024 * 1024)
                    print(f"[{total_files:4d}] Extraido: {rel_path} ({mb_size:.2f} MB)" if mb_size >= 1.0 else f"[{total_files:4d}] Extraido: {rel_path} ({file_size} B)")

        # Iniciar no raiz
        root_children = fs.files.get("/", (None, None))[1]
        if root_children is None:
            root_children = fs.root_directory.dict

        extract_tree(root_children)

        print("-" * 60)
        print(f"Sucesso! Total extraido: {total_files} arquivos, {total_bytes / (1024 * 1024):.2f} MB.")

def main():
    parser = argparse.ArgumentParser(description="Extrai arquivos de uma imagem .exfat do PS5.")
    parser.add_argument("image", help="Caminho para o arquivo .exfat")
    parser.add_argument("-o", "--output", help="Diretorio de saida (opcional)")
    args = parser.parse_args()

    image_path = os.path.abspath(args.image)
    if args.output:
        output_dir = os.path.abspath(args.output)
    else:
        base_name = os.path.splitext(os.path.basename(image_path))[0]
        output_dir = os.path.join(os.path.dirname(image_path), base_name)

    extract_exfat(image_path, output_dir)

if __name__ == "__main__":
    main()
