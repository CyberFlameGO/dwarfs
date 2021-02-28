dwarfsextract(1) -- extract DwarFS image
========================================

## SYNOPSIS

`dwarfsextract` -i *image* [-o *dir*] [*options*...]
`dwarfsextract` -i *image* -f *format* [-o *file*] [*options*...]<br>

## DESCRIPTION

**dwarfsextract** allows you to extract a DwarFS image, either directly
into another archive format file, or to a directory on disk.

To extract the filesystem image to a directory, you can use:

    dwarfsextract -i image.dwarfs -o output-directory

The output directory must exist.

You can also rewrite the contents of the filesystem image as another
archive type, for example, to write a tar archive, you can use:

    dwarfsextract -i image.dwarfs -o output.tar -f ustar

For a list of supported formats, see libarchive-formats(5).

If you want to compress the output archive, you can use a pipeline:

    dwarfsextract -i image.dwarfs -f ustar | gzip > output.tar.gz

You could also use this as an alternative way to extract the files
to disk:

    dwarfsextract -i image.dwarfs -f cpio | cpio -id

## OPTIONS

  * `-i`, `--input=`*file*:
    Path to the source filesystem.

  * `-o`, `--output=`*directory*`|`*file*:
    If no format is specified, this is the directory to which the contents
    of the filesystem should be extracted. If a format is specified, this
    is the name of the output archive. This option can be omitted, in which
    case the default is to extract the files to the current directory, or
    to write the archive data to stdout.

  * `-n`, `--num-workers=`*value*:
    Number of worker threads used for building the filesystem. This defaults
    to the number of processors available on your system. Use this option if
    you want to limit the resources used by `mkdwarfs`.

  * `-s`, `--cache-size=`*value*:
    Size of the block cache, in bytes. You can append suffixes (`k`, `m`, `g`)
    to specify the size in KiB, MiB and GiB, respectively. Note that this is
    not the upper memory limit of the process, as there may be blocks in
    flight that are not stored in the cache. Also, each block that hasn't been
    fully decompressed yet will carry decompressor state along with it, which
    can use a significant amount of additional memory.

  * `--log-level=`*name*:
    Specifiy a logging level.

  * `--help`:
    Show program help, including defaults, compression level detail and
    supported compression algorithms.

## AUTHOR

Written by Marcus Holland-Moritz.

## COPYRIGHT

Copyright (C) Marcus Holland-Moritz.

## SEE ALSO

mkdwarfs(1), dwarfs(1), libarchive-formats(5)