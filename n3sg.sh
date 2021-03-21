#!/bin/sh

markdown() {
  tail -n +$(($(sed -n '/---/,/---/p' $1 | wc -l)+1)) $1 | \
    awk -f ./awkdown -v esc=false
}

usage() {
  echo "n3sg - Netlify Simple Static Site Generator"
  echo "Usage:"
  echo "    ./n3sg.sh src dest site_name site_url"
  exit 1
}

test -n "$1" || usage
test -n "$2" || usage
test -n "$3" || usage
test -n "$4" || usage

src="$1"
dst="$2"
title="$3"
url="$4"

## Bootstrap directory structure from $src into $dst
for f in `cd $src && find . -type d ! -name '.' ! -path '*/_*'`; do
  mkdir -p "$dst/$f"
done

## Copy non-markdown files
for f in `cd $src && find . -type f ! -name '*.md' ! -name 'index.md' ! -name '.' ! -path '*/_*'`; do
  cp $src/$f $dst/$f
done

## Index page generation
cat $src/_header.html > $dst/index.html
[ -f $src/index.md ] && markdown $src/index.md >> $dst/index.html
echo "<ul>" >> $dst/index.html

## RSS generation
cat > $dst/rss.xml << EOF
<?xml version="1.0" encoding="UTF-8"?>
<rss version="2.0" xmlns:atom="http://www.w3.org/2005/Atom">
<channel>
<atom:link href="$url/rss.xml" rel="self" type="application/rss+xml" />
<title>$title</title>
<description></description>
<link>$url</link>
<lastBuildDate>$(date -R)</lastBuildDate>
EOF

## For all markdown files
for f in `cd $src && find . -type f -name '*.md' ! -name 'index.md' ! -name '.' ! -path '*/_*'`; do

  ## Meta extraction
  title=`sed -n '/---/,/---/p' $src/$f | grep title | cut -d':' -f2`
  author=`sed -n '/---/,/---/p' $src/$f | grep author | cut -d':' -f2`
  date=`git log -n 1 --date="format:%d-%m-%Y %H:%M:%SZ" --pretty=format:%ad -- $src/$f`
  page=$(basename $f .md)

  ## HTML
  cat $src/_header.html > $dst/$page.html
  echo "<h1>$title</h1>" >> $dst/$page.html
  echo "<p class=\"author\">Autor: <span class=\"value\">$author</span></p><p class=\"date\">Ostatnia zmiana: <span class=\"value\"><time>$date</time></span></p>" >> $dst/$page.html
  markdown $src/$f >> $dst/$page.html
  cat $src/_footer.html >> $dst/$page.html

  ## Add to index
  echo "<li class=\"link\"><a href=\"$page.html\">$title</a></li>" >> $dst/index.html

  ## Add to rss
  cat >> $dst/rss.xml << EOF
<item>
  <guid>$page</guid>
  <link>$url/$page.html</link>
  <pubDate>$date</pubDate>
  <title>$title</title>
  <description><![CDATA[
EOF
  markdown $src/$f >> $dst/rss.xml
  cat >> $dst/rss.xml << EOF
  ]]></description>
</item>
EOF

done

## Close tags
echo "</ul>" >> $dst/index.html
cat $src/_footer.html >> $dst/index.html
echo "</channel></rss>" >> $dst/rss.xml
