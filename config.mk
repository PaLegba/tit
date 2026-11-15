# PREFIX empty means auto: /usr/local, or $HOME/config/non-packaged on Haiku.
# Override with `make PREFIX=$HOME/.local`.
PREFIX ?=
CC = cc
OPT = -O2 -g
