#!/usr/bin/env bash

_nemu_completions()
{
    local curr prev

    curr=${COMP_WORDS[COMP_CWORD]}
    prev=${COMP_WORDS[COMP_CWORD-1]}

    COMPREPLY=()

    if [[ "$COMP_CWORD" == 1 ]]; then
        COMPREPLY=( $(compgen -W "-h --help -l --list -s --start -p --powerdown -f --force-stop -v --version" -- "$curr") )
    elif [[ "$COMP_CWORD" == 2 ]]; then
        case "$prev" in
            "-s"|"--start"|"-p"|"--powerdown"|"-f"|"--force-stop")
                COMPREPLY=( $(compgen -W "$(nemu -l)" -- "$curr") )
            ;;
            *)
            ;;
        esac
    fi

    return 0
} &&

complete -F _nemu_completions nemu
