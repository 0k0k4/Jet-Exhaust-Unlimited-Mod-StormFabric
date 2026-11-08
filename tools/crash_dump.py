"""Analyse d'un minidump Windows (MDMP) sans debugger.

Extrait : modules (base/taille/nom/version), threads + piles, exception, memoire
sauvegardee (MemoryList), et reconstitue une pile d'appels en cherchant les
adresses de retour dans les modules, puis les symbolise via le stream
FunctionTable (types 13) quand il est present.

Usage : python build/tests/crash_dump.py [chemin.dmp] [--rva 0x9B2D5D]
"""
import os
import struct
import sys

DEFAULT_DUMP = r'H:/Jeux/StormworksV1.15.23/Stormworks/crash.dmp'

# Fonctions du moteur certifiées par le projet (RVA du build 1.15.23). Elles
# donnent un NOM aux cadres de pile au lieu d'un simple « module+0x... ».
KNOWN_FUNCTIONS = {
    0x46e900: 'FindComponentDefinitionByName',
    0x46eec0: "allocation d'une ComponentDefinition (operator new(0x2d08))",
    0x315150: 'constructeur de ComponentDefinition',
    0x5f1ac0: 'ComponentRegistry::Clear',
    0x8b9ed0: 'destructeur du ring des descripteurs XML',
    0x8b9fa0: 'destructeur du ring des variantes (DestroyDefinition + delete(0x2d08))',
    0x87fdb0: 'destructeur de ComponentDefinition',
    0x958ae8: 'destroy_range(begin, stride, count, dtor)',
    0x958c28: 'operator new(size)',
    0x95881c: 'operator delete(ptr, size)',
    0x9b2d50: "str_free : liberation d'une chaine du moteur",
    0x898eb0: 'destructeur de chaine moteur {ptr,len,cap}',
    0x6ca1a0: 'fonction du moteur qui detruit une chaine (appel direct)',
    0x17cb00: 'str_reserve (reference 1.15.18)',
    0x957478: 'operator new du loader',
}

STREAM_NAMES = {
    3: 'ThreadList', 4: 'ModuleList', 5: 'MemoryList', 6: 'Exception',
    7: 'SystemInfo', 8: 'ThreadExList', 9: 'Memory64List', 10: 'CommentA',
    11: 'CommentW', 12: 'HandleData', 13: 'FunctionTable',
    14: 'UnloadedModuleList', 15: 'MiscInfo', 16: 'MemoryInfoList',
    17: 'ThreadInfoList', 18: 'HandleOperationList', 19: 'Token',
    20: 'JavaScriptData', 21: 'SystemMemoryInfo', 22: 'ProcessVmCounters',
    23: 'IptTrace', 24: 'ThreadNames',
}


class Dump:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b'MDMP':
            raise SystemExit('pas un minidump MDMP : %s' % d[:4])
        (self.version, self.nstreams, self.dirstream, self.checksum,
         self.timestamp, self.flags) = struct.unpack_from('<IIIIII', d, 4)
        self.streams = {}
        for i in range(self.nstreams):
            stype, size, rva = struct.unpack_from('<III', d, self.dirstream + i * 12)
            self.streams[stype] = (rva, size)
        self.images = {}          # {nom de module en minuscules: PEImage}
        self.modules = self._modules()
        self.functions = self._functions()
        self.ranges = self._memory_ranges()
        self.threads = self._threads()
        self.exception = self._exception()

    # -- streams -----------------------------------------------------------
    def _modules(self):
        """MINIDUMP_MODULE (108 o) : base, size, checksum, timestamp, nameRva,
        VersionInfo[13 dwords], CvRecord(8), MiscRecord(8), Reserved(16)."""
        out = []
        if 4 not in self.streams:
            return out
        rva, _ = self.streams[4]
        (count,) = struct.unpack_from('<I', self.data, rva)
        for k in range(count):
            off = rva + 4 + k * 108
            base, size, _ck, ts, namerva = struct.unpack_from('<QIIII', self.data, off)
            # VS_FIXEDFILEINFO commence a off+24 : Signature, StrucVersion,
            # FileVersionMS/LS, ProductVersionMS/LS ...
            fv_ms, fv_ls = struct.unpack_from('<II', self.data, off + 24 + 8)
            ln, = struct.unpack_from('<I', self.data, namerva)
            name = self.data[namerva + 4:namerva + 4 + ln].decode('utf-16-le', 'replace')
            out.append({'base': base, 'size': size, 'ts': ts, 'name': name,
                        'version': '%d.%d.%d.%d' % (fv_ms >> 16, fv_ms & 0xFFFF,
                                                    fv_ls >> 16, fv_ls & 0xFFFF)})
        return out

    def _functions(self):
        """MINIDUMP_FUNCTION_TABLE_STREAM : en-tete de 24 o (SizeOfHeader,
        SizeOfDescriptor, SizeOfNativeDescriptor, SizeOfFunctionEntry,
        NumberOfDescriptors, SizeOfAlignPad), puis SizeOfAlignPad octets de
        bourrage, puis les MINIDUMP_FUNCTION_TABLE_DESCRIPTOR (32 o).

        ATTENTION : sur les dumps reels ce stream est souvent ecrit dans un
        format proprietaire (Windows Error Reporting / MiniDumpWriteDump
        selon la version). On VALIDE donc chaque descripteur (adresses
        canoniques, plage coherente, base dans un module) et on abandonne
        proprement au premier incoherent : mieux vaut aucune symbolisation
        qu'une table de symboles fantaisistes."""
        out = []
        if 13 not in self.streams:
            return out
        rva, size = self.streams[13]
        try:
            hdr, descsz, _native, entrysz, ndesc, align = struct.unpack_from('<6I', self.data, rva)
        except struct.error:
            return out
        if hdr not in (16, 24, 32) or descsz not in (24, 32) or not 0 < ndesc < 0x10000:
            return out
        base_desc = rva + hdr + align
        if entrysz not in (8, 12) or base_desc + ndesc * descsz > len(self.data):
            return out
        for k in range(ndesc):
            off = base_desc + k * descsz
            minimum, maximum, base, nentries, pad = struct.unpack_from('<QQQII', self.data, off)
            if not (minimum < maximum) or not self.module_of(minimum):
                return []                      # descripteur incoherent -> on abandonne
            out.append((minimum, maximum))
        return out

    def _memory_ranges(self):
        """MemoryList (5) et Memory64List (9) -> {start: (rva, size)}."""
        out = {}
        if 9 in self.streams:                      # Memory64List
            rva, _ = self.streams[9]
            nranges, datarva = struct.unpack_from('<QQ', self.data, rva)
            off = rva + 16
            cur = datarva
            for _ in range(nranges):
                start, size = struct.unpack_from('<QQ', self.data, off)
                out[start] = (cur, size)
                off += 16
                cur += size
        if 5 in self.streams:                      # MemoryList
            rva, _ = self.streams[5]
            (nranges,) = struct.unpack_from('<I', self.data, rva)
            for k in range(nranges):
                start, dsize, drva = struct.unpack_from('<QII', self.data, rva + 4 + k * 16)
                out.setdefault(start, (drva, dsize))
        return out

    def _threads(self):
        """MINIDUMP_THREAD (48 o) : ThreadId, SuspendCount, PriorityClass,
        Priority, Teb, Stack(StartOfMemoryRange, MemoryLocation 8),
        ThreadContext(DataSize, Rva)."""
        out = []
        if 3 not in self.streams:
            return out
        rva, _ = self.streams[3]
        (count,) = struct.unpack_from('<I', self.data, rva)
        for k in range(count):
            off = rva + 4 + k * 48
            (tid, susp, priocls, prio, teb, stack_start, stack_dsize, stack_rva,
             ctx_size, ctx_rva) = struct.unpack_from('<IIIIQQIIII', self.data, off)
            out.append({
                'tid': tid, 'teb': teb, 'stack_start': stack_start,
                'stack_rva': stack_rva, 'stack_size': stack_dsize,
                'ctx_rva': ctx_rva, 'ctx_size': ctx_size,
                'context': self._context(ctx_rva),
            })
        return out

    def _context(self, rva):
        """CONTEXT AMD64 : Rip @0xF8, Rsp @0x98, Rbp @0xA0, Rax..R15."""
        names = ['Rax', 'Rcx', 'Rdx', 'Rbx', 'Rsp', 'Rbp', 'Rsi', 'Rdi',
                 'R8', 'R9', 'R10', 'R11', 'R12', 'R13', 'R14', 'R15']
        regs = struct.unpack_from('<16Q', self.data, rva + 0x78)
        ctx = dict(zip(names, regs))
        (ctx['Rip'],) = struct.unpack_from('<Q', self.data, rva + 0xF8)
        (ctx['EFlags'],) = struct.unpack_from('<I', self.data, rva + 0x44)
        return ctx

    def _exception(self):
        """MINIDUMP_EXCEPTION_STREAM (168 o) : ThreadId, __align, ExceptionRecord
        (Code, Flags, Record, Address, NumberParameters, __unused, Params[15])."""
        if 6 not in self.streams:
            return None
        rva, _ = self.streams[6]
        tid, _align = struct.unpack_from('<II', self.data, rva)
        code, flags, rec, addr, nparams = struct.unpack_from('<IIQQI', self.data, rva + 8)
        params = struct.unpack_from('<15Q', self.data, rva + 8 + 32)
        ctx_size, ctx_rva = struct.unpack_from('<II', self.data, rva + 160)
        return {'tid': tid, 'code': code, 'flags': flags, 'record': rec,
                'address': addr, 'nparams': nparams,
                'params': list(params[:nparams]),
                'context': self._context(ctx_rva)}

    # -- helpers -----------------------------------------------------------
    def module_of(self, va):
        for m in self.modules:
            if m['base'] <= va < m['base'] + m['size']:
                return m
        return None

    def symbol(self, va):
        """Nom de module + RVA, plus la fonction FunctionTable englobante."""
        m = self.module_of(va)
        if not m:
            return '0x%016X  <hors module>' % va
        rva = va - m['base']
        label = '%s+0x%X' % (os.path.basename(m['name']), rva)
        img = self.images.get(os.path.basename(m['name']).lower())
        fn = img.find(rva) if img else None
        if fn:
            name = KNOWN_FUNCTIONS.get(fn[0])
            return label + ('  [%s+0x%X]' % (name, rva - fn[0]) if name else
                            '  [fonction +0x%X..+0x%X]' % (fn[0], fn[1]))
        for beg, end in self.functions:
            if beg <= va < end:
                name = KNOWN_FUNCTIONS.get(beg - m['base'])
                label += '  [%s+0x%X]' % (name, va - beg) if name else                     '  [fonction +0x%X..+0x%X]' % (beg - m['base'], end - m['base'])
                break
        return label

    def read(self, va, size):
        """Lit `size` octets a l'adresse virtuelle `va` (None si absent du dump)."""
        for start, (rva, rsize) in self.ranges.items():
            if start <= va and va + size <= start + rsize:
                off = rva + (va - start)
                chunk = self.data[off:off + size]
                if len(chunk) == size:
                    return chunk
        return None

    def stack_words(self, start, size):
        raw = self.data[self.ranges[start][0]:self.ranges[start][0] + size] \
            if start in self.ranges else b''
        if not raw:
            return []
        n = len(raw) // 8
        return list(struct.unpack_from('<%dQ' % n, raw))

    def scan_stack(self, ctx, max_frames=40):
        """Reconstitution NAIVE : toute valeur de la pile qui pointe dans un module
        est un candidat. Utile en secours, mais melange pointeurs de donnees et
        adresses de retour : preferer walk_stack (deroulement .pdata)."""
        rsp = ctx['Rsp']
        candidates = []
        lo, hi = rsp, rsp + 0x4000
        for start, (rva, rsize) in sorted(self.ranges.items()):
            a, b = max(start, lo), min(start + rsize, hi)
            if a >= b:
                continue
            raw = self.data[rva + (a - start):rva + (b - start)]
            for i in range(0, len(raw) - 8 + 1, 8):
                (val,) = struct.unpack_from('<Q', raw, i)
                m = self.module_of(val)
                if m and 0 < val - m['base'] < m['size']:
                    candidates.append((a + i, val))
        return candidates[:max_frames]


# ---------------------------------------------------------------------------
# Deroulement de pile x64 exact, a partir du .pdata du binaire (RVA des
# RUNTIME_FUNCTION et UNWIND_INFO). Sans debugger, c'est la seule facon de
# separer les vraies adresses de retour des pointeurs de donnees posees sur la
# pile (une pile de jeu en est pleine : vtables, chaines, objets).
# ---------------------------------------------------------------------------
UWOP_PUSH_NONVOL = 0
UWOP_ALLOC_LARGE = 1
UWOP_ALLOC_SMALL = 2
UWOP_SET_FPREG = 3
UWOP_SAVE_NONVOL = 4
UWOP_SAVE_NONVOL_FAR = 5
UWOP_EPILOG = 6
UWOP_SPARE = 7
UWOP_SAVE_XMM128 = 8
UWOP_SAVE_XMM128_FAR = 9
UWOP_PUSH_MACHFRAME = 10

REGS = ['Rax', 'Rcx', 'Rdx', 'Rbx', 'Rsp', 'Rbp', 'Rsi', 'Rdi',
        'R8', 'R9', 'R10', 'R11', 'R12', 'R13', 'R14', 'R15']


class PEImage:
    """Vue minimale d'un PE sur disque : sections + repertoire d'exceptions."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        d = self.data
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        if d[pe:pe + 4] != b'PE\0\0':
            raise SystemExit('pas un PE : %s' % path)
        nsect, optsz = struct.unpack_from('<H12xH', d, pe + 6)[0], \
            struct.unpack_from('<H', d, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from('<H', d, opt)[0]
        self.image_base = struct.unpack_from('<Q', d, opt + 24)[0]
        ddoff = opt + (112 if magic == 0x20B else 96)
        self.dirs = [struct.unpack_from('<II', d, ddoff + i * 8) for i in range(16)]
        self.sections = []
        sect = opt + optsz
        for i in range(nsect):
            o = sect + i * 40
            name = d[o:o + 8].rstrip(b'\0').decode('latin-1')
            vsz, va, rsz, ra = struct.unpack_from('<IIII', d, o + 8)
            self.sections.append((name, va, vsz, ra, rsz))
        self._ranges = None

    def offset(self, rva):
        for _n, va, vsz, ra, rsz in self.sections:
            if va <= rva < va + max(vsz, rsz):
                return ra + (rva - va)
        return None

    def functions(self):
        """[(beginRva, endRva, unwindRva)] tries par RVA croissant."""
        if self._ranges is not None:
            return self._ranges
        rva, size = self.dirs[3]
        out = []
        off = self.offset(rva) if rva else None
        if off:
            for i in range(size // 12):
                b, e, u = struct.unpack_from('<III', self.data, off + i * 12)
                if b:
                    out.append((b, e, u))
        out.sort()
        self._ranges = out
        return out

    def find(self, rva):
        import bisect
        fns = self.functions()
        idx = bisect.bisect_right([f[0] for f in fns], rva) - 1
        if idx >= 0:
            b, e, u = fns[idx]
            if b <= rva < e:
                return (b, e, u)
        return None

    def unwind_info(self, rva):
        """Decode UNWIND_INFO -> (prologSize, frameReg, frameOff, codes, chained).
        `codes` est la liste (op, info, extra) dans l'ordre du prologue."""
        off = self.offset(rva)
        if off is None:
            return None
        d = self.data
        ver_flags, prolog, ncodes, frame = struct.unpack_from('<BBBB', d, off)
        version = ver_flags & 7
        flags = ver_flags >> 3
        if version != 1:
            return None
        codes = []
        p = off + 4
        for _ in range(ncodes):
            # UNWIND_CODE : octet 0 = CodeOffset (position dans le prologue),
            # octet 1 = (OpInfo << 4) | UnwindOp.
            _code_off, b1 = struct.unpack_from('<BB', d, p)
            p += 2
            op = b1 & 0x0F
            info = b1 >> 4
            extra = 0
            if op == UWOP_ALLOC_LARGE and info == 1:
                extra = struct.unpack_from('<I', d, p)[0]
                p += 4
            elif op in (UWOP_SAVE_NONVOL_FAR, UWOP_SAVE_XMM128_FAR):
                extra = struct.unpack_from('<I', d, p)[0]
                p += 4
            codes.append((op, info, extra))
        chained = None
        if flags & 0x4:                      # UNW_FLAG_CHAININFO
            p = (p + 3) & ~3
            chained = struct.unpack_from('<III', d, p)
        frame_reg = frame & 0x0F
        frame_off = frame >> 4
        return (prolog, frame_reg, frame_off, codes, chained)


def unwind_frame(image, ctx, read):
    """Applique UNWIND_INFO a `ctx` (dict de registres + Rip) et renvoie le
    contexte du cadre appelant, ou None si l'image ne couvre pas ce Rip."""
    f = image.find(ctx['Rip'] - 0)          # le Rip pointe dans la fonction
    if f is None:
        return None
    begin, end, urva = f
    # Rip peut etre exactement la fin (adresse de retour) : on recule d'un octet.
    if not (begin <= ctx['Rip'] < end):
        return None
    info = image.unwind_info(urva)
    if info is None:
        return None
    prolog, frame_reg, frame_off, codes, chained = info
    regs = dict(ctx)
    while True:
        for op, info_b, extra in codes:
            if op == UWOP_PUSH_NONVOL:
                val = read(regs['Rsp'], 8)
                if val is None:
                    return None
                regs[REGS[info_b]] = struct.unpack('<Q', val)[0]
                regs['Rsp'] += 8
            elif op == UWOP_ALLOC_LARGE:
                regs['Rsp'] += extra * 8 if info_b == 0 else extra
            elif op == UWOP_ALLOC_SMALL:
                regs['Rsp'] += info_b * 8 + 8
            elif op == UWOP_SET_FPREG:
                regs['Rsp'] = (regs[REGS[frame_reg]] - frame_off * 16) & 0xFFFFFFFFFFFFFFFF
            elif op == UWOP_SAVE_NONVOL:
                val = read(regs['Rsp'] + extra * 8, 8)
                if val is None:
                    return None
                regs[REGS[info_b]] = struct.unpack('<Q', val)[0]
            elif op == UWOP_SAVE_NONVOL_FAR:
                val = read(regs['Rsp'] + extra, 8)
                if val is None:
                    return None
                regs[REGS[info_b]] = struct.unpack('<Q', val)[0]
            elif op == UWOP_PUSH_MACHFRAME:
                base = regs['Rsp'] + (8 if info_b else 0)
                frame = read(base, 8 * 5)
                if frame is None:
                    return None
                rip, _cs, _efl, rsp, _ss = struct.unpack('<5Q', frame)
                regs['Rip'] = rip
                regs['Rsp'] = rsp
                return regs
            # UWOP_SAVE_XMM128* : sans effet sur les registres generaux
        if not chained:
            break
        begin, end, urva = chained
        info = image.unwind_info(urva)
        if info is None:
            return None
        _p, frame_reg, frame_off, codes, chained = info
    # Fin de prologue : [Rsp] = adresse de retour
    ret = read(regs['Rsp'], 8)
    if ret is None:
        return None
    regs['Rip'] = struct.unpack('<Q', ret)[0]
    regs['Rsp'] += 8
    return regs


def hexdump(buf, base=0):
    for i in range(0, len(buf), 16):
        chunk = buf[i:i + 16]
        print('  %08X  %-47s  %s' % (
            base + i, ' '.join('%02X' % b for b in chunk),
            ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    dump_path = args[0] if args else DEFAULT_DUMP
    d = Dump(dump_path)
    print('DUMP %s (%d octets, timestamp 0x%X, %d streams)'
          % (dump_path, len(d.data), d.timestamp, d.nstreams))
    print('streams : ' + ', '.join(
        '%s(%d)' % (STREAM_NAMES.get(t, t), s[1]) for t, s in sorted(d.streams.items())))

    print('\n=== MODULES (%d) ===' % len(d.modules))
    for m in d.modules:
        print('  %016X  %8X  %-32s %s'
              % (m['base'], m['size'], os.path.basename(m['name']), m['version']))

    ex = d.exception
    print('\n=== EXCEPTION ===')
    if ex:
        print('  thread %d  code 0x%08X  flags 0x%X  adresse 0x%016X'
              % (ex['tid'], ex['code'], ex['flags'], ex['address']))
        for i, p in enumerate(ex['params']):
            print('    param[%d] = 0x%016X' % (i, p))
        print('  faute -> %s' % d.symbol(ex['address']))
        print('  registres : ' + '  '.join(
            '%s=%016X' % (k, v) for k, v in ex['context'].items()))
        if ex['code'] == 0xC0000005 and len(ex['params']) >= 2:
            op = 'LECTURE' if ex['params'][0] == 0 else ('ECRITURE' if ex['params'][0] == 1 else
                                                         'EXECUTION' if ex['params'][0] == 8 else
                                                         '?%d' % ex['params'][0])
            print('  acces %s de l adresse 0x%016X' % (op, ex['params'][1]))

    print('\n=== THREADS (%d) ===' % len(d.threads))
    for t in d.threads:
        c = t['context']
        tag = '  <-- FAUTIF' if ex and t['tid'] == ex['tid'] else ''
        print('  tid %-6d rip=%016X rsp=%016X rbp=%016X  %s%s'
              % (t['tid'], c['Rip'], c['Rsp'], c['Rbp'], d.symbol(c['Rip']), tag))

    exe_arg = [a for a in sys.argv[1:] if a.endswith('.exe')]
    exe_path = exe_arg[0] if exe_arg else \
        r'H:/Jeux/StormworksV1.15.23/Stormworks/stormworks64.exe'
    images = {}
    if os.path.isfile(exe_path):
        images[os.path.basename(exe_path).lower()] = PEImage(exe_path)
    d.images = images

    if ex:
        # Le CONTEXTE DU THREAD dans ThreadList n'est PAS celui de la faute : au
        # moment du dump, le thread fautif execute le writer de minidump (ntdll).
        # La pile a explorer est donc celle du contexte de l'Exception stream.
        print('\n=== PILE AU MOMENT DE LA FAUTE (thread %d, rsp=%016X) ==='
              % (ex['tid'], ex['context']['Rsp']))
        ctx = dict(ex['context'])
        for depth in range(30):
            m = d.module_of(ctx['Rip'])
            name = os.path.basename(m['name']).lower() if m else ''
            img = images.get(name)
            ctx_rva = None
            if m and img:
                ctx_rva = ctx['Rip'] - m['base']
                where = ''
            else:
                where = ''
            print('  #%-2d rip=%016X rsp=%016X  %s%s'
                  % (depth, ctx['Rip'], ctx['Rsp'], d.symbol(ctx['Rip']), where))
            if not (m and img) or ctx_rva is None:
                # Pas de .pdata hors du binaire du jeu : heuristique (adresse de
                # retour en haut de pile) puis arret si elle ne pointe nulle part.
                nxt = d.read(ctx['Rsp'], 8)
                if nxt is None:
                    print('  ... (memoire non sauvegardee)')
                    break
                nrip = struct.unpack('<Q', nxt)[0]
                if not d.module_of(nrip):
                    break
                ctx = dict(ctx, Rip=nrip, Rsp=ctx['Rsp'] + 8)
                continue
            nxt_rva = unwind_frame(img, dict(ctx, Rip=ctx_rva), d.read)
            if nxt_rva is None:
                print('  ... (deroulement impossible : epilogue ou donnees)')
                break
            # unwind_frame renvoie l'adresse de retour telle qu'elle est
            # empilee : c'est deja une adresse virtuelle absolue.
            ctx = dict(nxt_rva)
        print('\n=== PILE (balayage naif, en secours) ===')
        for va, val in d.scan_stack(ex['context']):
            print('  [%016X] %016X  %s' % (va, val, d.symbol(val)))
        print('\n=== PILE COURANTE DU THREAD (writer de dump) ===')
        bad = [t for t in d.threads if t['tid'] == ex['tid']]
        if bad:
            for va, val in d.scan_stack(bad[0]['context'], max_frames=12):
                print('  [%016X] %016X  %s' % (va, val, d.symbol(val)))

    print('\n=== MEMOIRE SAUVEGARDEE (%d plages) ===' % len(d.ranges))
    for start, (rva, size) in sorted(d.ranges.items()):
        print('  %016X - %016X  (%6X o)  -> fichier 0x%X'
              % (start, start + size, size, rva))


if __name__ == '__main__':
    main()
