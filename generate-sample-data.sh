#!/usr/bin/env bash

set -Eeuo pipefail

rm -rf sample-data

mkdir sample-data
for i in {0..10000}; do
  touch "sample-data/$i.file"
done

for i in {0..200}; do
  mkdir "sample-data/$i.dir"

  for j in {0..100}; do
    touch "sample-data/$i.dir/$j.file"
  done

  for j in {0..25}; do
    mkdir "sample-data/$i.dir/$j.dir"

    for k in {0..10}; do
      touch "sample-data/$i.dir/$j.dir/$k.file"
    done

    for k in {0..10}; do
      mkdir "sample-data/$i.dir/$j.dir/$k.dir"
    done
  done
done
