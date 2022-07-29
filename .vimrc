syntax on
set number
set expandtab
set tabstop=2
set shiftwidth=2
set cc=81
hi ColorColumn ctermbg=58
if has('python')
  map <C-I> :pyf /usr/share/clang/clang-format-10/clang-format.py<cr>
"  imap <C-I> <c-o>:pyf /usr/share/clang/clang-format-10/clang-format.py<cr>
elseif has('python3')
  map <C-I> :py3f /usr/share/clang/clang-format-10/clang-format.py<cr>
"  imap <C-I> <c-o>:py3f /usr/share/clang/clang-format-10/clang-format.py<cr>
endif

let g:ale_lint_on_save = 1
let g:ale_fix_on_save = 1

