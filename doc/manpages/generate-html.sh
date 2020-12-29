#!/bin/sh

mkdir -p html
STYLE='/^<style type="text\/css">/a\       body    { margin-left: auto; margin-right: auto; width: 60em; padding-left: 1em; padding-right: 1em; background: cornsilk }'
STYLE2='/^<style type="text\/css">/a\       html    { background: aliceblue }'
cat dinit.8         | groff -mandoc -Thtml | sed -e "$STYLE" -e "$STYLE2" > html/dinit.8.html
cat dinitcheck.8    | groff -mandoc -Thtml | sed -e "$STYLE" -e "$STYLE2" > html/dinitcheck.8.html
cat dinitctl.8      | groff -mandoc -Thtml | sed -e "$STYLE" -e "$STYLE2" > html/dinitctl.8.html
cat dinit-service.5 | groff -mandoc -Thtml | sed -e "$STYLE" -e "$STYLE2" > html/dinit-service.5.html
