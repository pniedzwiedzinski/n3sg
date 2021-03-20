# Netlify Simple Static Site Generator

n3sg is a simple shell script for generating static sites supporting frontmatter

## Goal

The goal for this project is to have an extremely fast zero-dependency program
for generating multi-page static sites and also is easily compatible with
[Netlify CMS](https://www.netlifycms.org/).

## Usage

```
./n3sg.sh src dest
```

Copy `n3sg` and `awkdown` directly into your repo or use this repo as a template

## It's pretty fast

![Image of deploys list - it builds in under 20s](deploys.png)
![Image of logs - n3sg run in 73ms](build.png)
