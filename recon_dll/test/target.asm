; A function with the exact 15-byte prologue of hknpWorld::castRay: returns rcx + rdx.
.code
TargetFn PROC
    mov qword ptr [rsp+8], rbx
    mov qword ptr [rsp+10h], rbp
    mov qword ptr [rsp+18h], rsi
    push rdi
    sub rsp, 20h
    lea rax, [rcx+rdx]
    add rsp, 20h
    pop rdi
    mov rbx, qword ptr [rsp+8]
    mov rbp, qword ptr [rsp+10h]
    mov rsi, qword ptr [rsp+18h]
    ret
TargetFn ENDP
END
