# hexaOS default environment
export LANG=en_US.UTF-8
# User-installed programs (e.g. Claude Code lives in ~/.local/bin)
export PATH="$HOME/.local/bin:$PATH"
[ -z "$XDG_RUNTIME_DIR" ] && export XDG_RUNTIME_DIR=/run/user/$(id -u)

# Colored prompt
export PS1='\[\e[1;36m\]\u@hexaos\[\e[0m\]:\[\e[1;34m\]\w\[\e[0m\]\$ '
alias ll='ls -la'
