pkgname=vxwm
pkgver=2.3
pkgrel=1
pkgdesc="Versatile X Window Manager for x11 forked from dwm"
url="http://codeberg.org/wh1tepearl/vxwm/"
arch=('i686' 'x86_64' 'aarch64')
license=('MIT')
options=(zipman)
depends=('libx11' 'libxinerama')
install=vxwm.install
source=(git+https://github.com/orangeman432/vxwm.git
	vxwm.desktop
	start-vxwm
	modules.h
	config.h
	niri.c)
source=(${source[@]} ${_patches[@]})

build() {
  cd $srcdir/$pkgname


  for p in "${_patches[@]}"; do
    echo "=> $p"
    patch < ../$p || return 1
  done

  cp $srcdir/modules.h modules.h
  cp $srcdir/config.h config.h
  cp $srcdir/niri.c niri.c

  sed -i 's/CPPFLAGS =/CPPFLAGS +=/g' config.mk
  sed -i 's/^CFLAGS = -g/#CFLAGS += -g/g' config.mk
  sed -i 's/^#CFLAGS = -std/CFLAGS += -std/g' config.mk
  sed -i 's/^LDFLAGS = -g/#LDFLAGS += -g/g' config.mk
  sed -i 's/^#LDFLAGS = -s/LDFLAGS += -s/g' config.mk
  make X11INC=/usr/include/X11 X11LIB=/usr/lib/X11
}

package() {
  cd $srcdir/$pkgname
  make PREFIX=/usr DESTDIR=$pkgdir install
  install -m644 -D LICENSE $pkgdir/usr/share/licenses/$pkgname/LICENSE
  install -m644 -D README.md $pkgdir/usr/share/doc/$pkgname/README.md
  install -m644 -D $srcdir/vxwm.desktop $pkgdir/usr/share/xsessions/vxwm.desktop
  install -m655 -D $srcdir/start-vxwm $pkgdir/usr/bin/start-vxwm
}
md5sums=('SKIP'
         'f187782edcd0e11ddc17fa5802c38c4a'
         '6c1152a2522a5d618d6483987c6e2220'
         '6da8d82288d09f493e1e0c4347e939d8'
         'ca4b4b9bc03921bcdeb03cdbe2cffa63'
         '6271a2867ad625aa7e06f97e95a9cc02')
