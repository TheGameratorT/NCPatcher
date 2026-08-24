# bash completion for ncpatcher
#
# Completes the subcommand first, then that subcommand's flags plus the global
# ones, which are accepted on either side of it.

_ncpatcher()
{
	local cur prev words cword
	if declare -F _init_completion >/dev/null 2>&1; then
		_init_completion || return
	else
		cur="${COMP_WORDS[COMP_CWORD]}"
		prev="${COMP_WORDS[COMP_CWORD-1]}"
		words=("${COMP_WORDS[@]}")
		cword=$COMP_CWORD
	fi

	local globals='-C --project --rom --out -D --define --var --toolchain -j --jobs
		-v --verbose --verbose-tag --color --message-format --result
		--log --no-log -h --help --version'
	local commands='build clean restore config migrate modules rom version'

	# Options taking a value are completed from the value, not the option list.
	case "$prev" in
		-C|--project)
			_filedir -d
			return
			;;
		--rom)
			# Either a .nds or an extracted directory, so both are offered.
			_filedir
			return
			;;
		--out)
			_filedir
			return
			;;
		--result|--log|-o|--output)
			_filedir
			return
			;;
		--color)
			COMPREPLY=($(compgen -W 'auto always never' -- "$cur"))
			return
			;;
		--message-format)
			COMPREPLY=($(compgen -W 'human json' -- "$cur"))
			return
			;;
		--verbose-tag)
			COMPREPLY=($(compgen -W 'build section elf patch library linking symbols nolib all' -- "$cur"))
			return
			;;
		--variant)
			return
			;;
		-j|--jobs|-D|--define|--var|--toolchain)
			return
			;;
	esac

	# Which subcommand, if any, has been named already.
	local command='' sub='' i
	for ((i = 1; i < cword; i++)); do
		case "${words[i]}" in
			build|clean|restore|config|migrate|modules|rom|version)
				command="${words[i]}"
				;;
			dump)
				[[ $command == config || $command == modules ]] && sub="${words[i]}"
				;;
			validate|path)
				[[ $command == config ]] && sub="${words[i]}"
				;;
			list|explain)
				[[ $command == modules ]] && sub="${words[i]}"
				;;
			info|extract|pack)
				[[ $command == rom ]] && sub="${words[i]}"
				;;
		esac
		[[ -n $command && -n $sub ]] && break
	done

	local extra=''
	case "$command" in
		build)   extra='--variant --all-variants' ;;
		clean)   extra='--backups' ;;
		migrate) extra='--write' ;;
		config)
			if [[ -z $sub ]]; then
				COMPREPLY=($(compgen -W 'dump validate path' -- "$cur"))
				return
			fi
			[[ $sub == dump ]] && extra='--explain --json'
			;;
		modules)
			if [[ -z $sub ]]; then
				COMPREPLY=($(compgen -W 'list dump explain' -- "$cur"))
				return
			fi
			[[ $sub == dump ]] && extra='-o --output'
			;;
		rom)
			if [[ -z $sub ]]; then
				COMPREPLY=($(compgen -W 'info extract pack' -- "$cur"))
				return
			fi
			# extract and pack each take a directory.
			if [[ $sub == extract || $sub == pack ]] && [[ $cur != -* ]]; then
				_filedir -d
				return
			fi
			;;
	esac

	if [[ $cur == -* ]]; then
		COMPREPLY=($(compgen -W "$globals $extra" -- "$cur"))
	elif [[ -z $command ]]; then
		COMPREPLY=($(compgen -W "$commands" -- "$cur"))
	fi
}

complete -F _ncpatcher ncpatcher
