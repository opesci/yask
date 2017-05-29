#!/bin/bash

git pull upstream develop
git push origin develop
git rebase -i develop
