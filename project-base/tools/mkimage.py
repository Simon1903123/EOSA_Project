#!/usr/bin/env python3
"""
mkimage.py - impacchetta i programmi ELF in un'immagine IMG1 (versione BIG).

Uso (lo stesso schema del Makefile della base):

    python3 tools/mkimage.py --output build/fs.img CARTELLA

Lo script prende DA SOLO tutti i file *.axf contenuti nella cartella.
Il nome del programma e' il nome del file senza ".axf"
(print_plain.axf -> "print_plain"). I file sono presi in ordine alfabetico,
cosi' l'immagine (e l'ordine di `ls`) e' sempre la stessa.

Cosa fa:
    1. cerca i file *.axf nella cartella;
    2. controlla nomi, quantita' e file leggibili;
    3. calcola dove mettere ogni file dentro l'immagine;
    4. costruisce l'immagine: header (20 byte) + directory (24 byte per
       programma) + i file ELF copiati byte per byte, allineati a 4 byte;
    5. rilegge l'immagine con le stesse regole che usera' il kernel (autocontrollo);
    6. solo alla fine scrive il file di output.

In caso di errore non viene scritto nessun file e lo script esce con stato 2
(messaggio "mkimage.py: error: <motivo>" su stderr, come fa argparse).

Formato IMG1 (tutti i numeri sono interi senza segno a 32 bit, little-endian):

    HEADER (20 byte)
      0..3    magic            "IMG1" (49 4d 47 31)
      4..7    version          1
      8..11   image_size       dimensione totale dell'immagine
      12..15  count            numero di programmi (1..32)
      16..19  directory_offset dove inizia la directory (sempre 20)

    DIRECTORY (count voci da 24 byte)
      0..15   name             nome + NUL, il resto a zero
      16..19  offset           dove inizia il file ELF (multiplo di 4)
      20..23  size             dimensione esatta del file ELF (>= 40)

    FILE ELF, uno dopo l'altro, ciascuno che inizia a un indirizzo multiplo di 4.
"""

import argparse
import os
import re
import struct
import sys

# ---------------------------------------------------------------------------
# Costanti del formato IMG1 (sezione 6.1 dell'assignment)
# ---------------------------------------------------------------------------
IMG_MAGIC = b"IMG1"            # byte 49 4d 47 31
IMG_VERSION = 1
HEADER_SIZE = 20               # dimensione dell'header
ENTRY_SIZE = 24                # dimensione di una voce di directory
NAME_FIELD_SIZE = 16           # 15 caratteri al massimo + NUL
MAX_PROGRAMS = 32
MAX_IMAGE_SIZE = 0x00200000    # 2 MiB: 0x00200000..0x003fffff
MIN_FILE_SIZE = 40             # ogni file nell'immagine ha almeno 40 byte
ALIGNMENT = 4                  # ogni file inizia a un multiplo di 4
PROGRAM_EXTENSION = ".axf"      # i programmi compilati sono file .axf (formato ELF)

# Nome valido: da 1 a 15 caratteri tra A-Z a-z 0-9 _ -
NAME_RE = re.compile(r"[A-Za-z0-9_-]{1,15}")


def align_up(value, alignment=ALIGNMENT):
    """Arrotonda 'value' per eccesso al multiplo di 'alignment'."""
    return (value + alignment - 1) // alignment * alignment


# ---------------------------------------------------------------------------
# Passo 1: ricerca dei programmi nella cartella
# ---------------------------------------------------------------------------
def scan_directory(parser, directory):
    """
    Prende tutti i file *.axf direttamente dentro 'directory' (senza entrare
    nelle sottocartelle). Il nome del programma e' il nome del file senza
    ".axf". Restituisce [(nome, percorso), ...] in ordine alfabetico.
    """
    if not os.path.isdir(directory):
        parser.error("'%s' is not a directory" % directory)
    try:
        filenames = sorted(os.listdir(directory))    # sorted: ordine sempre uguale
    except OSError as exc:
        parser.error("cannot read directory '%s': %s" % (directory, exc.strerror))

    pairs = []
    for filename in filenames:
        if filename.startswith("."):                 # file nascosti: ignorati
            continue
        stem, extension = os.path.splitext(filename)
        if extension != PROGRAM_EXTENSION:               # .c, .o, .map, ... : ignorati
            continue
        path = os.path.join(directory, filename)
        if not os.path.isfile(path):                 # una cartella chiamata x.axf
            continue
        pairs.append((stem, path))

    if not pairs:
        parser.error(
            "no %s files found in '%s' (this folder must contain the compiled "
            "programs, not the .c sources)" % (PROGRAM_EXTENSION, directory)
        )
    return pairs


# ---------------------------------------------------------------------------
# Passo 2: controllo dei nomi e lettura dei file
# ---------------------------------------------------------------------------
def load_entries(parser, pairs):
    """
    Controlla ogni (nome, percorso) e legge il file. Restituisce una lista di
    (nome, byte_del_file). Ogni problema viene segnalato con parser.error(),
    che stampa "mkimage.py: error: <motivo>" ed esce con stato 2.
    """
    entries = []
    for name, path in pairs:
        # I nomi sono unici perche' vengono da file diversi della stessa
        # cartella, quindi non serve controllare i duplicati.
        if not NAME_RE.fullmatch(name):
            parser.error(
                "invalid program name '%s' (file '%s'): use 1 to 15 characters "
                "from A-Z a-z 0-9 _ -" % (name, path)
            )
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError as exc:
            parser.error("cannot read '%s': %s" % (path, exc.strerror))

        # Nota: NON controlliamo che sia un ELF valido. Il kernel deve
        # controllarlo da solo (B1), e i test del kit mettono nell'immagine
        # anche file volutamente rotti.
        if len(data) < MIN_FILE_SIZE:
            parser.error(
                "'%s' is too small (%d bytes, at least %d required)"
                % (path, len(data), MIN_FILE_SIZE)
            )
        entries.append((name, data))
    return entries


# ---------------------------------------------------------------------------
# Passo 3 e 4: calcolo del layout e costruzione dell'immagine
# ---------------------------------------------------------------------------
def build_image(entries):
    """Restituisce l'immagine completa (bytearray). ValueError se e' troppo grande."""
    count = len(entries)
    directory_offset = HEADER_SIZE                       # la directory e' sempre a 20
    directory_end = directory_offset + count * ENTRY_SIZE
    contents_start = align_up(directory_end)             # qui puo' iniziare il primo file

    # 3) Dove va ogni file: uno dopo l'altro, ognuno allineato a 4.
    offsets = []
    cursor = contents_start
    for _name, data in entries:
        offsets.append(cursor)
        cursor = align_up(cursor + len(data))

    # image_size = fine dell'ULTIMO file (senza il padding finale)
    image_size = offsets[-1] + len(entries[-1][1])
    if image_size > MAX_IMAGE_SIZE:
        raise ValueError(
            "image too large: %d bytes (maximum is %d)" % (image_size, MAX_IMAGE_SIZE)
        )

    # 4) Immagine tutta a zero: cosi' il padding e i byte dopo il NUL dei nomi
    #    sono gia' zero senza doverli scrivere.
    image = bytearray(image_size)

    # Header (20 byte)
    struct.pack_into(
        "<4sIIII", image, 0,
        IMG_MAGIC, IMG_VERSION, image_size, count, directory_offset,
    )

    # Directory (24 byte per voce). "16s" riempie con NUL i byte mancanti del nome.
    for index, ((name, data), offset) in enumerate(zip(entries, offsets)):
        struct.pack_into(
            "<16sII", image, directory_offset + index * ENTRY_SIZE,
            name.encode("ascii"), offset, len(data),
        )

    # File ELF copiati byte per byte, senza nessuna modifica.
    for (_name, data), offset in zip(entries, offsets):
        image[offset:offset + len(data)] = data

    return image


# ---------------------------------------------------------------------------
# Passo 5: autocontrollo (le stesse regole che applichera' il kernel in C)
# ---------------------------------------------------------------------------
def verify_image(image):
    """Rilegge l'immagine come farebbe il kernel. ValueError se una regola e' violata."""
    total = len(image)
    if total < HEADER_SIZE:
        raise ValueError("image shorter than the header")

    magic, version, image_size, count, dir_off = struct.unpack_from("<4sIIII", image, 0)

    if magic != IMG_MAGIC:
        raise ValueError("bad magic")
    if version != IMG_VERSION:
        raise ValueError("bad version")
    if image_size != total or not (HEADER_SIZE <= image_size <= MAX_IMAGE_SIZE):
        raise ValueError("bad image_size")
    if not (1 <= count <= MAX_PROGRAMS):
        raise ValueError("bad count")
    if dir_off % 4 != 0 or dir_off < HEADER_SIZE:
        raise ValueError("bad directory_offset")

    dir_end = dir_off + count * ENTRY_SIZE
    if dir_end > image_size:
        raise ValueError("directory outside the image")
    contents_start = align_up(dir_end)

    names = set()
    ranges = []
    for i in range(count):
        base = dir_off + i * ENTRY_SIZE
        raw_name = bytes(image[base:base + NAME_FIELD_SIZE])
        offset, size = struct.unpack_from("<II", image, base + NAME_FIELD_SIZE)

        nul = raw_name.find(b"\0")
        if nul < 0:
            raise ValueError("entry %d: name without NUL" % i)
        try:
            name = raw_name[:nul].decode("ascii")
        except UnicodeDecodeError:
            raise ValueError("entry %d: bad name" % i)
        if not NAME_RE.fullmatch(name):
            raise ValueError("entry %d: bad name" % i)
        if name in names:
            raise ValueError("entry %d: duplicate name" % i)
        names.add(name)

        if offset % 4 != 0:
            raise ValueError("entry %d: offset not aligned" % i)
        if size < MIN_FILE_SIZE:
            raise ValueError("entry %d: size too small" % i)
        if offset < contents_start:
            raise ValueError("entry %d: file overlaps the directory" % i)
        if offset + size > image_size:
            raise ValueError("entry %d: file outside the image" % i)
        ranges.append((offset, offset + size))

    ranges.sort()
    for (_s1, e1), (s2, _e2) in zip(ranges, ranges[1:]):
        if s2 < e1:
            raise ValueError("two files overlap")


# ---------------------------------------------------------------------------
# Passo 6: scrittura del file di output
# ---------------------------------------------------------------------------
def write_output(parser, output, image):
    """Scrive l'immagine. Prima su un file temporaneo, poi lo rinomina."""
    try:
        out_dir = os.path.dirname(output)
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)      # crea build/ se manca
        tmp_path = output + ".tmp"
        with open(tmp_path, "wb") as f:
            f.write(image)
        os.replace(tmp_path, output)
    except OSError as exc:
        parser.error("cannot write '%s': %s" % (output, exc.strerror))


# ---------------------------------------------------------------------------
# Programma principale
# ---------------------------------------------------------------------------
def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="mkimage.py",
        description="Pack the .axf programs of a directory into an IMG1 image.",
    )
    parser.add_argument("--output", required=True, metavar="OUTPUT",
                        help="output image, e.g. build/fs.img")
    parser.add_argument("directory", metavar="DIRECTORY",
                        help="folder containing the compiled .axf programs")
    args = parser.parse_args(argv)

    # Tutti i controlli avvengono PRIMA di scrivere qualsiasi output.
    pairs = scan_directory(parser, args.directory)
    if len(pairs) > MAX_PROGRAMS:
        parser.error("too many programs: %d (maximum is %d)" % (len(pairs), MAX_PROGRAMS))
    entries = load_entries(parser, pairs)

    try:
        image = build_image(entries)
    except ValueError as exc:
        parser.error(str(exc))

    try:
        verify_image(image)
    except ValueError as exc:
        # Non dovrebbe mai succedere: se succede, c'e' un bug in questo script.
        sys.exit("mkimage.py: internal error: %s" % exc)

    write_output(parser, args.output, image)
    print("IMG1: %s (%d bytes, %d programs)" % (args.output, len(image), len(entries)))


if __name__ == "__main__":
    main()
