# hexaOS default environment
export LANG=en_US.UTF-8
# User-installed programs (e.g. Claude Code lives in ~/.local/bin)
export PATH="$HOME/.local/bin:$PATH"
[ -z "$XDG_RUNTIME_DIR" ] && export XDG_RUNTIME_DIR=/run/user/$(id -u)
# /etc/profile picks vi; nano is easier
export EDITOR=nano VISUAL=nano
# less: keep colors, case-insensitive search, show the position
export LESS='-R -i -M'

# Everything below is for interactive bash only (the Terminal)
[ -n "$BASH_VERSION" ] || return 0
case $- in *i*) ;; *) return 0 ;; esac

# History: large, without duplicates, shared by every open terminal
HISTSIZE=10000
HISTFILESIZE=20000
HISTCONTROL=ignoreboth:erasedups
HISTTIMEFORMAT='%F %T  '
shopt -s histappend cmdhist checkwinsize globstar autocd cdspell dirspell 2>/dev/null

alias ls='ls --color=auto'
alias ll='ls -lah'
alias la='ls -A'
alias grep='grep --color=auto'
alias ..='cd ..'
alias ...='cd ../..'
alias open='xdg-open'

# Branch of the git repository in the current folder, for the prompt
__hx_git() {
    local b
    b=$(git symbolic-ref --short HEAD 2>/dev/null || git rev-parse --short HEAD 2>/dev/null) || return
    printf ' (%s)' "$b"
}

# Folder as a file:// URL (escape codes need spaces etc. percent-encoded)
__hx_url() {
    local s=$1 i c out=
    for ((i = 0; i < ${#s}; i++)); do
        c=${s:i:1}
        case $c in
            [a-zA-Z0-9/._~-]) out+=$c ;;
            *) printf -v c '%%%02X' "'$c"; out+=$c ;;
        esac
    done
    printf '%s' "$out"
}

# Runs before every prompt
__hx_prompt() {
    local st=$? err=
    history -a; history -n                       # share history between terminals
    # Tell the terminal the current folder (new windows open there) and set
    # the window title (the taskbar shows it)
    printf '\e]7;file://%s%s\e\\' "$HOSTNAME" "$(__hx_url "$PWD")"
    printf '\e]2;%s\e\\' "${PWD/#$HOME/\~}"
    [ "$st" != 0 ] && err="\[\e[1;31m\]✗ $st "
    # 133;A marks the prompt, so Ctrl+Shift+Z / X jump between commands
    PS1="\[\e]133;A\e\\\\\]$err\[\e[1;36m\]\u@\h\[\e[0m\]:\[\e[1;34m\]\w\[\e[0;33m\]\$(__hx_git)\[\e[0m\]\\$ "
}
PROMPT_COMMAND=__hx_prompt
