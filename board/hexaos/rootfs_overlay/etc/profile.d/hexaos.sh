# Ambiente padrao do hexaOS
export LANG=pt_BR.UTF-8
[ -z "$XDG_RUNTIME_DIR" ] && export XDG_RUNTIME_DIR=/run/user/$(id -u)

# Prompt colorido
export PS1='\[\e[1;36m\]\u@hexaos\[\e[0m\]:\[\e[1;34m\]\w\[\e[0m\]\$ '
alias ll='ls -la'
