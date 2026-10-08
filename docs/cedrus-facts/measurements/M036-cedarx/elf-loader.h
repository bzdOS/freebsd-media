/* SPDX-License-Identifier: BSD-2-Clause */
/* Load a foreign AArch64 plugin and resolve its public imports explicitly. */
struct foreign_elf {
    unsigned char *base;
    void *mapping;
    size_t mapping_size;
    Elf64_Sym *symbols;
    const char *strings;
    size_t symbol_count;
};
static struct foreign_elf plugin;
static void *foreign_resolve(const char *name);

static void *foreign_symbol(const char *name)
{
    for (size_t i = 1; i < plugin.symbol_count; i++)
        if (plugin.symbols[i].st_shndx != SHN_UNDEF &&
            strcmp(plugin.strings + plugin.symbols[i].st_name, name) == 0)
            return plugin.base + plugin.symbols[i].st_value;
    return NULL;
}

static void foreign_relocate(Elf64_Rela *rel, size_t bytes)
{
    for (size_t i = 0; i < bytes / sizeof(*rel); i++) {
        unsigned type = ELF64_R_TYPE(rel[i].r_info);
        size_t index = ELF64_R_SYM(rel[i].r_info);
        uintptr_t value;
        if (type == R_AARCH64_NONE)
            continue;
        if (type == R_AARCH64_RELATIVE) {
            value = (uintptr_t)plugin.base;
        } else if (type == R_AARCH64_ABS64 || type == R_AARCH64_GLOB_DAT ||
                   type == R_AARCH64_JUMP_SLOT) {
            Elf64_Sym *sym = &plugin.symbols[index];
            if (sym->st_shndx != SHN_UNDEF)
                value = (uintptr_t)plugin.base + sym->st_value;
            else {
                const char *name = plugin.strings + sym->st_name;
                value = (uintptr_t)foreign_resolve(name);
                if (!value && ELF64_ST_BIND(sym->st_info) != STB_WEAK) {
                    fprintf(stderr, "unresolved import: %s\n", name);
                    exit(2);
                }
            }
        } else {
            fprintf(stderr, "unsupported relocation: %u\n", type);
            exit(2);
        }
        *(uint64_t *)(plugin.base + rel[i].r_offset) = value + rel[i].r_addend;
    }
}

static void foreign_load(const char *path)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0)
        err(2, "plugin open");
    unsigned char *file = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file == MAP_FAILED)
        err(2, "plugin mmap");
    Elf64_Ehdr *eh = (void *)file;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_machine != EM_AARCH64 ||
        eh->e_type != ET_DYN || eh->e_ident[EI_CLASS] != ELFCLASS64)
        errx(2, "expected AArch64 shared object");
    Elf64_Phdr *ph = (void *)(file + eh->e_phoff);
    size_t end = 0;
    for (unsigned i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > end)
            end = ph[i].p_vaddr + ph[i].p_memsz;
    plugin.mapping_size = (end + 4095) & ~4095UL;
    plugin.mapping = mmap(NULL, plugin.mapping_size,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (plugin.mapping == MAP_FAILED)
        err(2, "plugin arena");
    plugin.base = plugin.mapping;
    Elf64_Dyn *dyn = NULL;
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD)
            memcpy(plugin.base + ph[i].p_vaddr, file + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_type == PT_DYNAMIC)
            dyn = (void *)(plugin.base + ph[i].p_vaddr);
    }
    Elf64_Shdr *sh = (void *)(file + eh->e_shoff);
    for (unsigned i = 0; i < eh->e_shnum; i++)
        if (sh[i].sh_type == SHT_DYNSYM)
            plugin.symbol_count = sh[i].sh_size / sizeof(Elf64_Sym);
    Elf64_Rela *rela = NULL, *jmp = NULL;
    size_t relasz = 0, jmpsz = 0;
    for (Elf64_Dyn *d = dyn; d && d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB: plugin.symbols = (void *)(plugin.base + d->d_un.d_ptr); break;
        case DT_STRTAB: plugin.strings = (void *)(plugin.base + d->d_un.d_ptr); break;
        case DT_RELA: rela = (void *)(plugin.base + d->d_un.d_ptr); break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmp = (void *)(plugin.base + d->d_un.d_ptr); break;
        case DT_PLTRELSZ: jmpsz = d->d_un.d_val; break;
        }
    }
    if (!plugin.symbols || !plugin.strings || !plugin.symbol_count)
        errx(2, "missing plugin symbols");
    foreign_relocate(rela, relasz);
    foreign_relocate(jmp, jmpsz);
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        int prot = ((ph[i].p_flags & PF_R) ? PROT_READ : 0) |
                   ((ph[i].p_flags & PF_W) ? PROT_WRITE : 0) |
                   ((ph[i].p_flags & PF_X) ? PROT_EXEC : 0);
        uintptr_t start = ph[i].p_vaddr & ~4095UL;
        size_t len = ((ph[i].p_vaddr + ph[i].p_memsz + 4095) & ~4095UL) - start;
        if (mprotect(plugin.base + start, len, prot) != 0)
            err(2, "plugin protect");
    }
    __builtin___clear_cache((char *)plugin.base,
                           (char *)plugin.base + plugin.mapping_size);
    munmap(file, st.st_size);
    close(fd);
    printf("PLUGIN loaded %zu bytes, %zu symbols\n", plugin.mapping_size,
           plugin.symbol_count);
}
