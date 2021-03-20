#!/bin/sh

# jekyll-cache is cached by netlify plugin
mkdir -p .jekyll-cache
export PATH="$PWD/.jekyll-cache:$PATH"


## lowdown
if [ ! -f ../.jekyll-cache/lowdown ]; then
  cd "lowdown-0.8.3"
  ./configure
  make
  cp lowdown ../../.jekyll-cache
  cd ..
fi


markdown() {
  tail -n +$(($(sed -n '/---/,/---/p' $1 | wc -l)+1)) $1 | \
    lowdown --html-no-skiphtml --html-no-escapehtml
}

usage() {
  echo "n3sg - Netlify Simple Static Site Generator"
  echo "Usage:"
  echo "    ./n3sg.sh src dest"
  exit 1
}

test -n "$1" || usage
test -n "$2" || usage

src="$1"
dst="$2"

for f in `cd $src && find . -type d ! -name '.' ! -path '*/_*'`; do
  mkdir -p "$dst/$f"
done

cat $src/_header.html > $dst/index.html
[ -f $src/index.md ] && markdown $src/index.md >> $dst/index.html
echo "<ul>" >> $dst/index.html

for f in `cd $src && find . -type f -name '*.md' ! -name 'index.md' ! -name '.' ! -path '*/_*'`; do
  title=`sed -n '/---/,/---/p' $src/$f | grep title | cut -d':' -f2`
  author=`sed -n '/---/,/---/p' $src/$f | grep author | cut -d':' -f2`
  page=$(basename $f .md)
  cat $src/_header.html > $dst/$page.html
  echo "<h1>$title</h1>" >> $dst/$page.html
  echo "<p class=\"author\">Autor: $author</p>" >> $dst/$page.html
  markdown $src/$f >> $dst/$page.html
  cat $src/_footer.html >> $dst/$page.html

  echo "<li class=\"link\"><a href=\"$page.html\">$title</a></li>" >> $dst/index.html

done

cat $src/_footer.html >> $dst/index.html
