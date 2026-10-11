#!/usr/bin/env bash
set -euo pipefail
prefix=/opt/atrinik-appimage
mkdir -p /build "${prefix}/tools"
python3 - <<'PY'
import hashlib,json,pathlib
p=pathlib.Path('/inputs'); lock=json.loads((p/'packaging.lock.json').read_text())
for e in lock['sources']+lock['tools']+[dict(lock['runtime'],name='runtime')]+lock['license_inputs']:
 f=p/('sources' if e in lock['sources'] else 'tools')/(e['name']+'.tar.gz' if e in lock['sources'] else e['name'])
 assert hashlib.sha256(f.read_bytes()).hexdigest()==e['sha256'],f
PY
for archive in /inputs/sources/*.tar.gz; do tar -xzf "${archive}" -C /build; done
export CFLAGS="-ffile-prefix-map=/build=. -fdebug-prefix-map=/build=. -ffile-prefix-map=/opt/atrinik-appimage=/usr"
export CXXFLAGS="${CFLAGS}"
export CMAKE_PREFIX_PATH=${prefix} LD_LIBRARY_PATH=${prefix}/lib PKG_CONFIG_PATH=${prefix}/lib/pkgconfig
build() {
 local source=$1; shift
 cmake -S "/build/${source}" -B "/build/${source}-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${prefix}" -DCMAKE_INSTALL_LIBDIR=lib "$@"
 cmake --build "/build/${source}-build" --parallel 4
 cmake --install "/build/${source}-build"
}
(cd /build/openssl-3.5.5; ./Configure linux-x86_64 shared --prefix=/usr --libdir=lib --openssldir=/etc/ssl; make -j4; make DESTDIR=/build/openssl-install install_sw; cp -a /build/openssl-install/usr/. "${prefix}/")
build c-ares-1.34.6 -DCARES_SHARED=ON -DCARES_STATIC=OFF -DCARES_BUILD_TESTS=OFF
build curl-8.18.0 -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF -DBUILD_TESTING=OFF \
 -DCURL_USE_OPENSSL=ON -DOPENSSL_ROOT_DIR="${prefix}" -DENABLE_ARES=ON \
 -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none -DCURL_CA_FALLBACK=ON -DCURL_USE_LIBPSL=OFF -DCURL_ZSTD=OFF -DCURL_BROTLI=OFF
build SDL3-3.4.2 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_KMSDRM=OFF -DSDL_WAYLAND_LIBDECOR=OFF
build SDL3_image-3.4.0 -DSDLIMAGE_VENDORED=OFF -DSDLIMAGE_AVIF=OFF -DSDLIMAGE_WEBP=OFF -DSDLIMAGE_TIF=OFF -DSDLIMAGE_JXL=OFF -DSDLIMAGE_TESTS=OFF -DSDLIMAGE_SAMPLES=OFF
build SDL3_ttf-3.2.2 -DSDLTTF_VENDORED=OFF -DSDLTTF_SAMPLES=OFF -DSDLTTF_TESTS=OFF
build SDL3_mixer-3.2.4 -DSDLMIXER_DEPS_SHARED=OFF -DSDLMIXER_EXAMPLES=OFF \
 -DSDLMIXER_AIFF=ON -DSDLMIXER_WAVE=ON -DSDLMIXER_VOC=ON -DSDLMIXER_AU=ON -DSDLMIXER_FLAC=OFF -DSDLMIXER_GME=OFF -DSDLMIXER_INSTALL=ON -DSDLMIXER_MIDI=OFF \
 -DSDLMIXER_MOD=OFF -DSDLMIXER_MP3=ON -DSDLMIXER_MP3_DRMP3=ON -DSDLMIXER_MP3_MPG123=OFF \
 -DSDLMIXER_OPUS=ON -DSDLMIXER_TESTS=OFF -DSDLMIXER_VORBIS_STB=ON \
 -DSDLMIXER_VORBIS_TREMOR=OFF -DSDLMIXER_VORBIS_VORBISFILE=OFF -DSDLMIXER_WAVPACK=OFF
# Preserve complete upstream license texts, including licenses of embedded decoder sources.
python3 - <<'PY'
import json,pathlib
lock=json.loads(pathlib.Path('/inputs/packaging.lock.json').read_text())
for e in lock['sources']:
 root=pathlib.Path('/build')/e['source_directory']; target=pathlib.Path('/opt/atrinik-appimage/share/licenses')/e['name']/'LICENSE';target.parent.mkdir(parents=True,exist_ok=True)
 files=sorted(p for p in root.rglob('*') if p.is_file() and any(p.name.lower().startswith(k) for k in ('license','copying','copyright','notice')))
 files += sorted(p for p in root.rglob('*.h') if p.name.startswith(('dr_', 'stb_')))
 if root/e['license_file'] not in files: raise RuntimeError('missing license '+e['name'])
 with target.open('wb') as out:
  for f in files:
   out.write(('\n===== '+str(f.relative_to(root))+' =====\n').encode());out.write(f.read_bytes());out.write(b'\n')
PY
for tool in linuxdeploy appimagetool; do
 chmod +x "/inputs/tools/${tool}"
 mkdir -p "${prefix}/tools/${tool}.AppDir"
 (cd "${prefix}/tools/${tool}.AppDir"; "/inputs/tools/${tool}" --appimage-extract >/dev/null; mv squashfs-root/* .; rmdir squashfs-root)
 printf '#!/bin/sh\nexec /opt/atrinik-appimage/tools/%s.AppDir/AppRun "$@"\n' "${tool}" > "${prefix}/tools/${tool}"
 chmod +x "${prefix}/tools/${tool}"
done
install -m 755 /inputs/tools/runtime "${prefix}/tools/runtime"


for name in linuxdeploy appimagetool runtime; do
 dest=${name}; [[ ${name} != runtime ]] || dest=appimage-runtime
 mkdir -p "${prefix}/share/licenses/${dest}"
 cp "/inputs/tools/${name}-LICENSE" "${prefix}/share/licenses/${dest}/LICENSE"
done

python3 - <<'PYLICENSE'
import json,pathlib,tarfile,hashlib
lock=json.loads(pathlib.Path('/inputs/packaging.lock.json').read_text())
notice=pathlib.Path('/opt/atrinik-appimage/share/licenses/appimage-runtime/LICENSE')
with notice.open('ab') as out:
 for e in lock['runtime_sources']:
  archive=pathlib.Path('/inputs/runtime-sources')/e['filename']
  assert hashlib.sha256(archive.read_bytes()).hexdigest()==e['sha256']
  with tarfile.open(archive) as source:
   files=[m for m in source.getmembers() if m.isfile() and (any(pathlib.PurePosixPath(m.name).name.lower().startswith(k) for k in ('license','copying','copyright','notice','lgpl')) or m.name.endswith('/'+e['license_file']))]
   if not files: raise RuntimeError('missing runtime component licenses: '+e['name'])
   for m in files:
    out.write(('\n===== '+e['name']+' '+m.name+' =====\n').encode());out.write(source.extractfile(m).read());out.write(b'\n')
PYLICENSE
