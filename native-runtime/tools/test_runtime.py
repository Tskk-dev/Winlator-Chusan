"""End-to-end synthetic x86 tests for the compiled block runtime; no game input."""
import argparse
from datetime import datetime,timezone
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import uuid
import capstone
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM

from prepare_runtime import generate, ROOT
from run_runtime import build, freeze_source, run_stop
from test_device import select_device

sys.path.insert(0, str(ROOT / "tests"))
from test_inventory import fixture


def event_program(manual=False, named=False, blocking=False):
    imports = ["CreateEventW", "WaitForSingleObject", "SetEvent", "ResetEvent", "CloseHandle", "GetLastError"]
    code, failures = bytearray(), []

    def push(value):
        return b"\x68" + struct.pack("<I", value)

    def call(index, args=b""):
        code.extend(args + b"\xff\x15" + struct.pack("<I", 0x401880 + 4*index))

    def expect(value):
        code.extend(b"\x3d" + struct.pack("<I", value) + b"\x0f\x85")
        failures.append(len(code));code.extend(bytes(4))

    def wait(register, value):
        call(1, push(0) + register);expect(value)

    name = 0x401700 if named else 0
    call(0, push(name) + push(0 if named or blocking else 1) + push(int(manual or named)) + push(0))
    code.extend(b"\x89\xc3")  # EBX holds the first opaque event handle.
    if blocking:
        call(1, push(0xffffffff) + b"\x53")
    elif named:
        call(0, push(name) + push(1) + push(0) + push(0));code.extend(b"\x89\xc6")
        call(5);expect(183)
        wait(b"\x56", 258)  # Opening an existing event ignores the new initial state.
        call(2, b"\x53");expect(1)
        wait(b"\x56", 0);wait(b"\x53", 0)
        call(4, b"\x53");expect(1)
        wait(b"\x56", 0)  # The second handle keeps the shared object alive.
        call(0, push(0)*4);code.extend(b"\x89\xc7")
        call(2, b"\x53");expect(0)  # Reusing a slot must not revive a stale handle.
        call(5);expect(6);wait(b"\x57",258)
        call(4,b"\x57");expect(1);call(4,b"\x56");expect(1)
    else:
        wait(b"\x53", 0);wait(b"\x53", 0 if manual else 258)
        call(3,b"\x53");expect(1);wait(b"\x53",258)
        call(2,b"\x53");expect(1);wait(b"\x53",0)
        call(4,b"\x53");expect(1)
        wait(b"\x53",0xffffffff)
        call(2,b"\x53");expect(0);call(5);expect(6)
    code.extend(b"\xb8\x0d\x60\x00\x00\xc3")
    failure = len(code);code.extend(b"\xb8\xad\x0b\x00\x00\xc3")
    for offset in failures:
        struct.pack_into("<i",code,offset,failure-offset-4)
    return {0:code,0x700:"event-\u96ea\0".encode("utf-16-le")}, imports


def mutex_program(kind):
    imports=["CreateMutexA","CreateMutexW","WaitForSingleObject","ReleaseMutex","CloseHandle","GetLastError",
             "CreateEventA","CreateEventW","SetEvent","ResetEvent"]
    code,failures=bytearray(),[]
    def push(value):return b"\x68"+struct.pack("<I",value)
    def call(index,args=b""):code.extend(args+b"\xff\x15"+struct.pack("<I",0x401880+4*index))
    def expect(value):
        code.extend(b"\x3d"+struct.pack("<I",value)+b"\x0f\x85");failures.append(len(code));code.extend(bytes(4))
    def create(index,owner,name,register=b"\x89\xc3"):
        call(index,push(name)+push(owner)+push(0))
        code.extend(bytes.fromhex("85c00f84"));failures.append(len(code));code.extend(bytes(4));code.extend(register)
    def wait(register=b"\x53"):call(2,push(0)+register);expect(0)
    def release(register=b"\x53",success=True):call(3,register);expect(int(success))
    if kind=="named":
        create(0,0,0x401700);wait()
        create(1,1,0x401740,b"\x89\xc6");call(5);expect(183)
        release(b"\x56");release(success=False);call(5);expect(288)
        call(4,b"\x53");expect(1);wait(b"\x56");call(4,b"\x56");expect(1)
        create(0,1,0x401700);call(5);expect(0)
        call(2,push(0)+b"\x56");expect(0xffffffff);call(5);expect(6)
        release();release(success=False);call(5);expect(288);call(4,b"\x53");expect(1)
    elif kind=="namespace":
        call(6,push(0x401700)+push(0)*3);code.extend(b"\x89\xc3")
        call(1,push(0x401740)+push(0)*2);expect(0);call(5);expect(6)
        call(8,b"\x53");expect(1);wait();release(success=False);call(5);expect(6)
        call(4,b"\x53");expect(1);create(0,0,0x401700)
        call(7,push(0x401740)+push(0)*3);expect(0);call(5);expect(6)
        call(8,b"\x53");expect(0);call(9,b"\x53");expect(0);call(5);expect(6)
        release(success=False);call(5);expect(288);wait();release();call(4,b"\x53");expect(1)
    else:
        owner=kind=="owned";create(0 if owner else 1,int(owner),0)
        if not owner:release(success=False);call(5);expect(288)
        wait();release()
        if owner:release()
        release(success=False);call(5);expect(288)
        wait();call(4,b"\x53");expect(1)
        call(2,push(0)+b"\x53");expect(0xffffffff)
        release(success=False);call(5);expect(6)
    code.extend(bytes.fromhex("b80d600000c3"));failure=len(code);code.extend(bytes.fromhex("b8ad0b0000c3"))
    for offset in failures:struct.pack_into("<i",code,offset,failure-offset-4)
    assert len(code)<0x700
    return {0:code,0x700:b"mutex-shared\0",0x740:"mutex-shared\0".encode("utf-16-le")},imports


def calls_program(imports, operations,cdecl=frozenset()):
    code,branches=bytearray(),[]
    for symbol,args,expected in operations:
        if symbol=="read32":
            code.extend(b"\xa1"+struct.pack("<I",args[0]))
        elif symbol=="write32":
            code.extend(b"\xc7\x05"+struct.pack("<II",*args))
        else:
            for argument in reversed(args):
                code.extend(b"\x68"+struct.pack("<I",argument))
            code.extend(b"\xff\x15"+struct.pack("<I",0x401880+4*imports.index(symbol)))
            if symbol in cdecl:
                assert len(args)*4<128
                code.extend(bytes.fromhex("83c4")+bytes([len(args)*4]))
        if expected is None:
            continue
        code.extend(b"\x3d"+struct.pack("<I",expected)+b"\x0f\x85")
        branches.append(len(code));code.extend(bytes(4))
    code.extend(bytes.fromhex("b80d600000c3"))
    failure=len(code);code.extend(bytes.fromhex("b8ad0b0000c3"))
    for branch in branches:
        struct.pack_into("<i",code,branch,failure-branch-4)
    assert len(code)<0x800
    return {0:code},imports


def heap_program(aligned=True, double_free=False):
    imports=["_aligned_malloc" if aligned else "malloc","_aligned_free" if aligned else "free","_errno"]
    code,branches=bytearray(),[]
    def bad(condition):
        code.extend(b"\x0f"+bytes([condition]));branches.append(len(code));code.extend(bytes(4))
    def allocate(size):
        if aligned:code.extend(b"\x6a\x40")
        code.extend(b"\x68"+struct.pack("<I",size)+bytes.fromhex("ff158018400083c4")+bytes([8 if aligned else 4]))
    def release(register):
        code.extend(register+bytes.fromhex("ff158418400083c404"))
    allocate(32);code.extend(bytes.fromhex("89c385c0"));bad(0x84)
    code.extend(bytes.fromhex("83e0")+bytes([63 if aligned else 7]));bad(0x85)
    allocate(8);code.extend(bytes.fromhex("89c685c0"));bad(0x84)
    code.extend(bytes.fromhex("39d8"));bad(0x84)
    code.extend(bytes.fromhex("c70378563412c706efcdab898b033d78563412"));bad(0x85)
    release(b"\x53")
    if double_free:release(b"\x53")
    release(b"\x56");release(b"\x6a\x00")
    allocate(0xffffffff);code.extend(bytes.fromhex("85c0"));bad(0x85)
    code.extend(bytes.fromhex("ff15881840008b003d0c000000"));bad(0x85)
    code.extend(bytes.fromhex("b80d600000c3"))
    failure=len(code);code.extend(bytes.fromhex("b8ad0b0000c3"))
    for branch in branches:struct.pack_into("<i",code,branch,failure-branch-4)
    return {0:code},imports


def exception_program(kind):
    imports=["__std_exception_copy","__std_exception_destroy","malloc","free","_errno"]
    borrowed=kind=="borrowed";message=0 if kind=="null" else (0xdeadbeef if kind=="invalid" else 0x401700)
    operations=[]
    if kind=="oom":operations.append(("malloc",[0x0b000000],0x03000000))
    operations.append(("__std_exception_copy",[0x401720,0x401740],None))
    if kind=="oom":
        operations.extend([("read32",[0x401740],0x11111111),("read32",[0x401744],0x3322117f),
                           ("_errno",[],0x0fe50028),("read32",[0x0fe50028],12),("free",[0x03000000],None)])
    else:
        target=message if borrowed or kind=="null" else 0x03000000
        operations.extend([("read32",[0x401740],target),("read32",[0x401744],0x33221100+int(bool(message) and not borrowed))])
        if message and kind!="invalid":
            operations.extend([("write32",[message,0x41424344],None),
                               ("read32",[target],0x41424344 if borrowed else 0x6c796170)])
        operations.extend([("__std_exception_destroy",[0x401740],None),("read32",[0x401740],0),
                           ("read32",[0x401744],0x33221100),("malloc",[12],0x03000000),("free",[0x03000000],None)])
    patches,_=calls_program(imports,operations,frozenset(imports))
    patches.update({0x700:b"payload1234\0",0x720:struct.pack("<IB3B",message,int(not borrowed),0xaa,0xbb,0xcc),
                    0x740:struct.pack("<IB3B",0x11111111,0x7f,0x11,0x22,0x33)})
    return patches,imports


def calendar_program(year,invalid=False):
    code=bytearray.fromhex("6800174000ff158018400083c404")
    branches=[]
    expected=-1 if invalid else int(datetime(year,1,1,4,1,5,tzinfo=timezone.utc).timestamp())
    def compare(value):
        code.extend(b"\x3d"+struct.pack("<I",value&0xffffffff)+b"\x0f\x85")
        branches.append(len(code));code.extend(bytes(4))
    compare(expected);code.extend(bytes.fromhex("89d0"));compare(expected>>32)
    if invalid:
        code.extend(bytes.fromhex("ff15841840008b00"));compare(22)
    else:
        for index,value in ((0,5),(1,1),(5,year-1900),(6,(datetime(year,1,1).weekday()+1)%7),(7,0),(8,0)):
            code.extend(b"\xa1"+struct.pack("<I",0x401700+4*index));compare(value)
    code.extend(bytes.fromhex("b80d600000c3"))
    failure=len(code);code.extend(bytes.fromhex("b8ad0b0000c3"))
    for branch in branches:struct.pack_into("<i",code,branch,failure-branch-4)
    return {0:code,0x700:struct.pack("<9i",65,0,4,1,0,year-1900,0,0,0)},["_mktime64","_errno"]


def paddd_program(source=0x401710):
    code=bytearray.fromhex("f30f6f0500174000f30f6f0d1017400039c0f9660ffe05")
    code.extend(struct.pack("<I",source))
    code.extend(bytes.fromhex("660ffec1660ffec99c5af30f7f0520174000f30f7f0d3017400089d025d5080000"))
    branches=[]
    def compare(value):
        code.extend(b"\x3d"+struct.pack("<I",value)+b"\x0f\x85")
        branches.append(len(code));code.extend(bytes(4))
    compare(0x45)  # Defined arithmetic flags from CMP/STC must survive the vector additions.
    for index,value in enumerate((1,0x80000001,0x80000000,0xfedcba97,2,2,0,0xfdb97530)):
        code.extend(b"\xa1"+struct.pack("<I",0x401720+4*index));compare(value)
    code.extend(bytes.fromhex("b80d600000c3"))
    failure=len(code);code.extend(bytes.fromhex("b8ad0b0000c3"))
    for branch in branches:struct.pack_into("<i",code,branch,failure-branch-4)
    return {0:code,0x700:struct.pack("<4I",0xffffffff,0x7fffffff,0x80000000,0x01234567),
            0x710:struct.pack("<4I",1,1,0x80000000,0xfedcba98)},["GetLastError"]


def scalar_movsd_program(source=0x401723):
    code=bytearray.fromhex("565731f631fff30f6f0500174000f30f6f0d1017400039c0f9f20f1005")
    code.extend(struct.pack("<I",source))
    code.extend(bytes.fromhex("f30f7f0540174000f30f6f0500174000f20f10c1f30f7f0550174000f20f110d711740009c5a89d025d5080000"))
    branches=[]
    def compare(value):
        code.extend(b"\x3d"+struct.pack("<I",value)+b"\x0f\x85");branches.append(len(code));code.extend(bytes(4))
    compare(0x45)
    code.extend(bytes.fromhex("89f0"));compare(0)
    code.extend(bytes.fromhex("89f8"));compare(0)
    for address,values in ((0x401740,(0x05060708,0x01020304,0,0)),
                           (0x401750,(0x55555555,0x66666666,0x33333333,0x44444444)),
                           (0x401771,(0x55555555,0x66666666))):
        for lane,value in enumerate(values):
            code.extend(b"\xa1"+struct.pack("<I",address+4*lane));compare(value)
    code.extend(bytes.fromhex("be00174000bf90174000fca589f0"));compare(0x401704)
    code.extend(bytes.fromhex("89f8"));compare(0x401794)
    code.extend(bytes.fromhex("a190174000"));compare(0x11111111)
    code.extend(bytes.fromhex("b80d6000005f5ec3"))
    failure=len(code);code.extend(bytes.fromhex("b8ad0b00005f5ec3"))
    for branch in branches:struct.pack_into("<i",code,branch,failure-branch-4)
    assert len(code)<0x700
    return {0:code,0x700:struct.pack("<4I",0x11111111,0x22222222,0x33333333,0x44444444),
            0x710:struct.pack("<4I",0x55555555,0x66666666,0x77777777,0x88888888),
            0x723:struct.pack("<Q",0x0102030405060708)},["GetLastError"]


def scenarios():
    patches,imports=calls_program(["VirtualQuery"],[("VirtualQuery",[0x401000,0x401700,28],None)])
    yield "memory-query-unqualified-caller",patches,[],True,imports,"virtual_memory_query","0x0040100f"
    imports=["GetCurrentProcess","K32GetModuleInformation","SetLastError","GetLastError","GetSystemInfo","K32EnumProcessModules"]
    patches,_=calls_program(imports,[("GetCurrentProcess",[],0xffffffff),("SetLastError",[0x1234],None),
        ("K32GetModuleInformation",[0xffffffff,0x400000,0x401700,12],1),
        ("read32",[0x401700],0x400000),("read32",[0x401704],0x2000),("read32",[0x401708],0x401000),
        ("read32",[0x40170c],0xa5a5a5a5),("GetSystemInfo",[0x401740],None),
        *[("read32",[0x401740+4*i],value) for i,value in enumerate((0,4096,0x10000,0x0fffffff,1,1,586,0x10000,6))],
        ("read32",[0x401764],0xa5a5a5a5),
        ("K32EnumProcessModules",[0xffffffff,0,0,0x401788],1),("read32",[0x401788],4),
        ("K32EnumProcessModules",[0xffffffff,0x401780,8,0x401788],1),
        ("read32",[0x401780],0x400000),("read32",[0x401784],0xa5a5a5a5),("GetLastError",[],0x1234)])
    patches[0x700]=bytes([0xa5])*16
    patches[0x740]=bytes([0xa5])*40
    patches[0x780]=bytes([0xa5])*12
    yield "main-module-information",patches,[],True,imports,"entry_returned","0x0000600d"
    code=bytearray();branches=[]
    for state,expected in ((0,0),(1,0),(2,1),(4,1),(6,1)):
        code.extend(b"\xc7\x05"+struct.pack("<II",0x40170c,state)+bytes.fromhex("b900174000ff15801840003d"))
        code.extend(struct.pack("<I",expected)+b"\x0f\x85");branches.append(len(code));code.extend(bytes(4))
    code.extend(bytes.fromhex("b80d600000c3"));failure=len(code);code.extend(bytes.fromhex("b8ad0b0000c3"))
    for branch in branches:struct.pack_into("<i",code,branch,failure-branch-4)
    yield "basic-ios-fail-state",{0:code},[],True,{
        "dll":"MSVCP140.dll","names":["?fail@ios_base@std@@QBE_NXZ"]},"entry_returned","0x0000600d"
    for kind in ("normal","wrong-facet"):
        imports=["??0_Lockit@std@@QAE@H@Z","??1_Lockit@std@@QAE@XZ",
                 "?id@?$codecvt@DDU_Mbstatet@@@std@@2V0locale@2@A","??Bid@locale@std@@QAEIXZ",
                 "?always_noconv@codecvt_base@std@@QBE_NXZ"]
        code=bytearray.fromhex("6a00b900174000ff1580184000b900174000ff1584184000a188184000");branches=[]
        def facet_compare(value):
            code.extend(b"\x3d"+struct.pack("<I",value)+b"\x0f\x85");branches.append(len(code));code.extend(bytes(4))
        facet_compare(0x0fe52110)
        code.extend(bytes.fromhex("8bc8ff158c184000"));facet_compare(1)
        code.extend(b"\xb9"+struct.pack("<I",0x401700 if kind=="wrong-facet" else 0x0fe53040))
        code.extend(bytes.fromhex("ff1590184000"));facet_compare(1)
        code.extend(bytes.fromhex("b94030e50fa14030e50fff500c"));facet_compare(1)
        code.extend(bytes.fromhex("b80d600000c3"));failure=len(code);code.extend(bytes.fromhex("b8ad0b0000c3"))
        for branch in branches:struct.pack_into("<i",code,branch,failure-branch-4)
        yield "classic-codecvt-"+kind,{0:code},[],True,{"dll":"MSVCP140.dll","names":imports},(
            "cpp_codecvt" if kind=="wrong-facet" else "entry_returned"),(
            "0x00401700" if kind=="wrong-facet" else "0x0000600d")
    fiopen="?_Fiopen@std@@YAPAU_iobuf@@PBDHH@Z"
    for kind in ("read-only","missing","parent","write-mode"):
        filename={"missing":b"absent.conf\0","parent":b"../fixture.conf\0"}.get(kind,b"fixture.conf\0")
        operations=[(fiopen,[0x401700,2 if kind=="write-mode" else 1,0x40],
                     0x03000000 if kind=="read-only" else 0)]
        if kind=="read-only":
            operations.extend([("_get_stream_buffer_pointers",[0x03000000,0x401740,0x401744,0x401748],None),
                               ("read32",[0x401740],0x03000004),("read32",[0x401744],0x03000000),
                               ("read32",[0x401748],0x03000008),("read32",[0x03000008],0),
                               ("fclose",[0x03000000],0),(fiopen,[0x401700,1,0x40],0x03000000),
                               ("fclose",[0x03000000],0)])
        elif kind!="write-mode":
            operations.extend([("_errno",[],0x0fe50028),("read32",[0x0fe50028],22 if kind=="parent" else 2)])
        imports=[fiopen,"fclose","_errno","_get_stream_buffer_pointers"];patches,_=calls_program(imports,operations,frozenset(imports))
        patches[0x700]=filename
        yield "content-file-"+kind,patches,[],True,{"dll":"MSVCP140.dll","names":imports},(
            "file_open_mode" if kind=="write-mode" else "entry_returned"),(
            "0x00000002" if kind=="write-mode" else "0x0000600d")
    ios_ctor="??0?$basic_ios@DU?$char_traits@D@std@@@std@@IAE@XZ"
    istream_ctor="??0?$basic_istream@DU?$char_traits@D@std@@@std@@QAE@PAV?$basic_streambuf@DU?$char_traits@D@std@@@1@_N@Z"
    streambuf_ctor="??0?$basic_streambuf@DU?$char_traits@D@std@@@std@@IAE@XZ"
    streambuf_init="?_Init@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IAEXXZ"
    streambuf_getloc="?getloc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QBE?AVlocale@2@XZ"
    for kind in ("state","reset","virtual"):
        code=bytearray.fromhex("b900174000ff1580184000");branches=[]
        def buffer_compare(value):
            code.extend(b"\x3d"+struct.pack("<I",value)+b"\x0f\x85");branches.append(len(code));code.extend(bytes(4))
        buffer_compare(0x401700)
        if kind=="virtual":code.extend(bytes.fromhex("6a00a100174000ff10"))
        if kind=="reset":
            for offset in range(0,0x38,4):
                code.extend(b"\xc7\x05"+struct.pack("<II",0x401700+offset,0x11223344))
            code.extend(bytes.fromhex("b900174000ff1584184000"))
        expected={0:0x11223344 if kind=="reset" else 0x0fe5201c,
                  0x34:0x11223344 if kind=="reset" else 0x03000000,0x38:0xa5a5a5a5}
        for offset in (4,0x14,0x24):
            expected.update({offset:0,offset+4:0,offset+8:0x401700+offset,offset+12:0x401704+offset})
        for offset,value in expected.items():
            code.extend(b"\xa1"+struct.pack("<I",0x401700+offset));buffer_compare(value)
        # Inline MSVC accessors dereference these aliases; they must reach the real cells.
        code.extend(bytes.fromhex("a10c174000c7002a000000a104174000"));buffer_compare(42)
        if kind=="state":
            code.extend(bytes.fromhex("6880174000b900174000ff1588184000"));buffer_compare(0x401780)
            for address,value in ((0x401780,0xa5a5a5a5),(0x401784,0x0fe53000),(0x0fe53004,3)):
                code.extend(b"\xa1"+struct.pack("<I",address));buffer_compare(value)
            code.extend(bytes.fromhex("b90030e50fa10030e50fff5008a10430e50f"));buffer_compare(2)
        code.extend(bytes.fromhex("b80d600000c3"));failure=len(code);code.extend(bytes.fromhex("b8ad0b0000c3"))
        for branch in branches:struct.pack_into("<i",code,branch,failure-branch-4)
        yield "basic-streambuf-"+kind,{0:code,0x700:bytes([0xa5])*64,0x780:bytes([0xa5])*8},[],True,{
            "dll":"MSVCP140.dll","names":[streambuf_ctor,streambuf_init,streambuf_getloc]},(
            "cpp_virtual_method" if kind=="virtual" else "entry_returned"),(
            "0x0fd40030" if kind=="virtual" else "0x0000600d")
    for kind in ("null","buffer","mode","virtual"):
        buffer=0x401700 if kind=="buffer" else 0
        code=bytearray.fromhex("b970164000ff1580184000")
        code.extend(b"\x6a\x00\x6a"+bytes([kind=="mode"])+b"\x68"+struct.pack("<I",buffer))
        code.extend(bytes.fromhex("b900164000ff1584184000"));branches=[]
        def stream_compare(value):
            code.extend(b"\x3d"+struct.pack("<I",value)+b"\x0f\x85");branches.append(len(code));code.extend(bytes(4))
        stream_compare(0x401600)
        if kind=="virtual":code.extend(bytes.fromhex("b9701640006a00a170164000ff10"))
        values={0:0x401740,4:0xa5a5a5a5,8:0,12:0,0x6c:0x58,0x70:0x0fe5200c,
                0x74:0xa5a5a5a5,0x78:0,0x7c:0 if buffer else 4,0x80:0,0x84:0x201,0x88:6,
                0x8c:0,0x90:0,0x94:0,0x98:0,0x9c:0,0xa0:0x03000000,0xa4:0xa5a5a5a5,
                0xa8:buffer,0xac:0,0xb0:0xa5a5a520,0xb4:0xa5a5a5a5}
        for offset,value in values.items():code.extend(b"\xa1"+struct.pack("<I",0x401600+offset));stream_compare(value)
        for address,value in ((0x03000004,0x0fe53000),(0x0fe53004,2),(0x0fe52100,0x43)):
            code.extend(b"\xa1"+struct.pack("<I",address));stream_compare(value)
        code.extend(bytes.fromhex("b80d600000c3"));failure=len(code);code.extend(bytes.fromhex("b8ad0b0000c3"))
        for branch in branches:struct.pack_into("<i",code,branch,failure-branch-4)
        assert len(code)<0x600
        data=bytearray([0xa5]*0xc0);struct.pack_into("<I",data,0,0x401740)
        stop,target={"mode":("cpp_stream_ctor_mode","0x00000001"),"virtual":("cpp_virtual_method","0x0fd40010")}.get(kind,("entry_returned","0x0000600d"))
        yield "basic-istream-"+kind,{0:code,0x600:data,0x740:struct.pack("<II",0,0x70)},[],True,{
            "dll":"MSVCP140.dll","names":[ios_ctor,istream_ctor]},stop,target
    for kind in ("normal","null","readonly","virtual","vtable-readonly","wrong-module"):
        address=0 if kind=="null" else 0x401700
        code=bytearray(b"\xb9"+struct.pack("<I",address)+bytes.fromhex("ff1580184000"));branches=[]
        def ios_compare(value):
            code.extend(b"\x3d"+struct.pack("<I",value)+b"\x0f\x85");branches.append(len(code));code.extend(bytes(4))
        ios_compare(address);code.extend(bytes.fromhex("89c8"));ios_compare(address)
        if kind=="virtual":code.extend(bytes.fromhex("6a00a100174000ff10"))
        if kind=="vtable-readonly":code.extend(bytes.fromhex("c7050420e50f00000000"))
        for offset in range(0,96,4):
            value=(0x0fe52004 if not offset else (0 if offset in (*range(8,0x34,4),0x38,0x3c) else
                   (0xa5a5a500 if offset==0x40 else 0xa5a5a5a5)))
            code.extend(b"\xa1"+struct.pack("<I",0x401700+offset));ios_compare(value)
        code.extend(bytes.fromhex("b80d600000c3"));failure=len(code);code.extend(bytes.fromhex("b8ad0b0000c3"))
        for branch in branches:struct.pack_into("<i",code,branch,failure-branch-4)
        stop,target={"null":("write_bounds","0x00000000"),"readonly":("write_permission","0x00401700"),
                     "virtual":("cpp_virtual_method","0x0fd40000"),"vtable-readonly":("write_permission","0x0fe52004"),
                     "wrong-module":("missing_import","0x00401880")}.get(kind,("entry_returned","0x0000600d"))
        imported={"dll":"KERNEL32.dll" if kind=="wrong-module" else "MSVCP140.dll","names":[ios_ctor]}
        yield "basic-ios-"+kind,{0:code,0x700:bytes([0xa5])*96},[],kind!="readonly",imported,stop,target
    modes=["GetErrorMode","SetErrorMode","SetLastError","GetLastError"]
    operations=[("SetLastError",[0x1234],None),("GetErrorMode",[],0),("SetErrorMode",[0x8002],0),
                ("GetErrorMode",[],0x8002),("SetErrorMode",[0x8003],0x8002),("SetErrorMode",[1],0x8003),
                ("GetErrorMode",[],1),("SetErrorMode",[0],1),("GetErrorMode",[],0),("GetLastError",[],0x1234)]
    patches,imports=calls_program(modes,operations)
    yield "process-error-mode",patches,[],False,imports,"entry_returned","0x0000600d"
    for suffix,value in (("alignment",4),("unknown",0x10)):
        patches,imports=calls_program(modes,[("SetErrorMode",[value],None)])
        yield "process-error-mode-"+suffix,patches,[],False,imports,"error_mode_flags",f"0x{value:08x}"
    for kind in ("owned","unowned","named","namespace"):
        patches,imports=mutex_program(kind)
        yield "mutex-"+kind,patches,[],False,imports,"entry_returned","0x0000600d"
    command_line="_get_narrow_winmain_command_line"
    patches,imports=calls_program([command_line],[(command_line,[],0x0fe51000),("read32",[0x0fe51000],0),
                                                  (command_line,[],0x0fe51000)])
    yield "winmain-empty-command-line",patches,[],False,imports,"entry_returned","0x0000600d"
    for kind,address in (("normal",0x401700),("null",0),("readonly",0x401700),("boundary",0x401fe0)):
        imports=["GetStartupInfoW","SetLastError","GetLastError"]
        operations=[("SetLastError",[0x1234],None),("GetStartupInfoW",[address],None)]
        operations.extend(("read32",[0x401700+offset],68 if not offset else 0) for offset in range(0,68,4))
        operations.extend([("read32",[0x4016fc],0xa5a5a5a5),("read32",[0x401744],0xa5a5a5a5),("GetLastError",[],0x1234)])
        patches,imports=calls_program(imports,operations)
        patches[0x6fc]=bytes([0xa5])*76
        stop,target=(("entry_returned","0x0000600d") if kind=="normal" else
                     (("write_bounds","0x00000000") if kind=="null" else ("write_permission",f"0x{address:08x}")))
        yield "startup-info-"+kind,patches,[],kind!="readonly",imports,stop,target
    for name,source,stop,target in (("simd-scalar-double",0x401723,"entry_returned","0x0000600d"),
                                    ("simd-scalar-double-memory",0x402000,"read_permission","0x00402000")):
        patches,imports=scalar_movsd_program(source)
        yield name,patches,[],True,imports,stop,target
    for kind in ("owned","borrowed","null","oom","invalid","readonly"):
        patches,imports=exception_program(kind)
        stop,target=("read_bounds","0xdeadbeef") if kind=="invalid" else (("write_permission","0x00401740") if kind=="readonly" else ("entry_returned","0x0000600d"))
        yield "exception-data-"+kind,patches,[],kind!="readonly",imports,stop,target
    loop = bytearray.fromhex("b800000000b905000000e8110000004975f8c3")
    loop.extend(bytes(0x20-len(loop)))
    loop.extend(bytes.fromhex("01c8c3"))
    yield "loop-call", {0: loop}, [], False, None, "entry_returned", "0x0000000f"
    yield "budget", {0: bytes.fromhex("ebfe")}, [], False, None, "block_limit", "0x00401000"
    yield "header-guard", {0: bytes.fromhex("bf00000000b82a000000aac3")}, [], False, None, "write_bounds", "0x00000000"
    yield "readonly", {0: bytes.fromhex("c705c01140002a000000c3")}, [], False, None, "write_permission", "0x004011c0"
    fp_control = bytes.fromhex("680000030068000001006a00ff155811400083c40cd93dc01140000fb705c0114000c3")
    yield "fp-control-53", {0: fp_control}, [], True, "_controlfp_s", "entry_returned", "0x0000027f"
    simd_copy = bytes.fromhex("f30f6f05c0114000f30f7f05d0114000a1dc114000c3")
    yield "simd-copy", {0: simd_copy, 0x1c0: struct.pack("<IIII", 1, 2, 3, 0x10203040)}, [], True, None, "entry_returned", "0x10203040"
    half_store = bytes.fromhex("f30f6f05c01140000f1305d01140000f1705d8114000a1dc114000c3")
    yield "simd-half-store", {0: half_store, 0x1c0: struct.pack("<IIII", 1, 2, 3, 0x10203040)}, [], True, None, "entry_returned", "0x10203040"
    for name,address,stop,target in (("simd-paddd",0x401710,"entry_returned","0x0000600d"),
                                     ("simd-paddd-alignment",0x401711,"guest_exception","0x0000000d"),
                                     ("simd-paddd-memory",0x402000,"read_permission","0x00402000")):
        patches,imports=paddd_program(address)
        yield name,patches,[],True,imports,stop,target
    critical = bytes.fromhex("6a0068a00f00006800134000ff155811400083c40ca104134000c3")
    yield "critical-init", {0: critical}, [], True, "__vcrt_InitializeCriticalSectionEx", "entry_returned", "0xffffffff"
    yield "critical-init-win32", {0:bytes.fromhex("6800134000ff1558114000a104134000c3")}, [], True, "InitializeCriticalSection", "entry_returned", "0xffffffff"
    yield "critical-init-ex", {0:critical.replace(bytes.fromhex("83c40c"),b"")}, [], True, "InitializeCriticalSectionEx", "entry_returned", "0xffffffff"
    yield "memset", {0:bytes.fromhex("6a0468ddccbbaa68c0114000ff155811400083c40ca1c0114000c3")}, [], True, "memset", "entry_returned", "0xdddddddd"
    yield "memset-guard", {0:bytes.fromhex("6a046a006a00ff1558114000c3")}, [], False, "memset", "write_bounds", "0x00000000"
    yield "memcpy", {0:bytes.fromhex("6a0868801240006890124000ff155811400083c40ca194124000c3"),
                     0x280:struct.pack("<II",0x12345678,0xaabbccdd)}, [], True, "memcpy", "entry_returned", "0xaabbccdd"
    yield "memmove", {0:bytes.fromhex("6a0768801240006881124000ff155811400083c40ca184124000c3"),
                      0x280:bytes(range(1,9))}, [], True, "memmove", "entry_returned", "0x07060504"
    tls_imports=["TlsAlloc","TlsGetValue","TlsSetValue","TlsFree","GetLastError","SetLastError"]
    operations=[("TlsAlloc",[],index) for index in range(65)]
    operations.extend([("TlsSetValue",[0,0x12345678],1),("TlsSetValue",[64,0x87654321],1),
                       ("SetLastError",[123],0),("TlsGetValue",[1],0),("GetLastError",[],0),
                       ("TlsGetValue",[0],0x12345678),("TlsGetValue",[64],0x87654321),
                       ("TlsFree",[0],1),("TlsAlloc",[],0),("TlsGetValue",[0],0),
                       ("TlsGetValue",[1088],0),("GetLastError",[],87),
                       ("TlsSetValue",[1088,1],0),("TlsFree",[1088],0)])
    tls_patches,tls_imports=calls_program(tls_imports,operations)
    yield "win32-tls", tls_patches, [], False, tls_imports, "entry_returned", "0x0000600d"
    cs_imports=["InitializeCriticalSection","EnterCriticalSection","TryEnterCriticalSection",
                "LeaveCriticalSection","DeleteCriticalSection"]
    cs_operations=[("InitializeCriticalSection",[0x401700],None),
                   ("EnterCriticalSection",[0x401700],None),("EnterCriticalSection",[0x401700],None),
                   ("read32",[0x401708],2),("read32",[0x40170c],1),
                   ("LeaveCriticalSection",[0x401700],None),("read32",[0x401708],1),
                   ("TryEnterCriticalSection",[0x401700],1),("read32",[0x401708],2),
                   ("LeaveCriticalSection",[0x401700],None),("LeaveCriticalSection",[0x401700],None),
                   ("read32",[0x401704],0xffffffff),("read32",[0x40170c],0),
                   ("DeleteCriticalSection",[0x401700],None)]
    cs_patches,cs_imports=calls_program(cs_imports,cs_operations)
    yield "critical-recursion",cs_patches,[],True,cs_imports,"entry_returned","0x0000600d"
    cs_patches,cs_imports=calls_program(cs_imports,cs_operations+[("EnterCriticalSection",[0x401700],None)])
    yield "critical-deleted",cs_patches,[],True,cs_imports,"critical_section_state","0x00401700"
    for name,aligned,double_free in (("heap-aligned",True,False),("heap-normal",False,False),("heap-double-free",True,True)):
        patches,imports=heap_program(aligned,double_free)
        yield name,patches,[],False,imports,("heap_invalid_free" if double_free else "entry_returned"),(None if double_free else "0x0000600d")
    for year,invalid in ((2015,False),(2200,False),(1969,True),(3001,True)):
        patches,imports=calendar_program(year,invalid)
        yield "calendar-"+str(year),patches,[],True,imports,"entry_returned","0x0000600d"
    for name,capacity,count,result,words in (
        ("limited",8,2,0,[0x004196ea,0xa5a50000]),
        ("short",8,7,0,[0x004196ea,0x00000042,0xa5a5a5a5]),
        ("truncated",3,0xffffffff,80,[0x004196ea,0xa5a50000]),
        ("exact",4,0xffffffff,0,[0x004196ea,0x00000042]),
        ("zero",8,0,0,[0xa5a50000]),
        ("range",3,3,None,[])):
        operations=[("wcsncpy_s",[0x401700,capacity,0x401740,count],result)]
        operations.extend(("read32",[0x401700+4*i],value) for i,value in enumerate(words))
        patches,imports=calls_program(["wcsncpy_s"],operations,{"wcsncpy_s"})
        patches.update({0x700:bytes([0xa5])*32,0x740:"\u96eaAB\0".encode("utf-16-le")})
        yield "wstring-"+name,patches,[],True,imports,("wstring_range_invalid_parameter" if name=="range" else "entry_returned"),("0x00401700" if name=="range" else "0x0000600d")
    pairs=[(0x401700,0x401700,0),(0x401700,0x401740,0xffffffff),
           (0x401740,0x401700,1),(0x401700,0x401780,0),(0x401780,0x401700,0),
           (0x401700,0x4017c0,0xffffffff),(0x4017c0,0x401740,1)]
    patches,imports=calls_program(["__std_type_info_compare"],
        [("__std_type_info_compare",[left,right],expected) for left,right,expected in pairs],{"__std_type_info_compare"})
    for offset,name in ((0x700,b".Alpha\0"),(0x740,b".Beta\0"),(0x780,b"!Alpha\0"),(0x7c0,b".\x80\0")):
        patches[offset]=struct.pack("<I",0xdeadbeef)+name
    yield "type-info",patches,[],False,imports,"entry_returned","0x0000600d"
    module = bytes.fromhex("6800134000ff1558114000c3")
    for suffix,api,encoding in (("","GetModuleHandleW","utf-16-le"),("-ansi","GetModuleHandleA","ascii")):
        yield "kernel-module"+suffix, {0: module, 0x300: "KERNEL32.DLL\0".encode(encoding)}, [], False, api, "entry_returned", "0x0fd80000"
        yield "main-module"+suffix, {0: bytes.fromhex("6a00ff1558114000c3")}, [], False, api, "entry_returned", "0x00400000"
    optional = bytes.fromhex("6800134000680000d80fff155811400064a134000000c3")
    yield "optional-export", {0: optional, 0x300: b"InitializeConditionVariable\0"}, [], False, "GetProcAddress", "entry_returned", "0x0000007f"
    for name,handle,symbol in (("filesystem-export-absent",0x0fd80000,b"CreateHardLinkW\0"),
                               ("filesystem-export-symlink-absent",0x0fd80000,b"CreateSymbolicLinkW\0"),
                               ("dynamic-export-unknown",0x0fd80000,b"createhardlinkw\0"),
                               ("dynamic-export-wrong-module",0x0fd80010,b"CreateHardLinkW\0")):
        code=bytes.fromhex("680013400068")+struct.pack("<I",handle)+bytes.fromhex("ff155811400085c0750764a134000000c3b8ad0b0000c3")
        positive=name.startswith("filesystem-export-")
        yield name,{0:code,0x300:symbol},[],False,"GetProcAddress",("entry_returned" if positive else "dynamic_export"),("0x0000007f" if positive else "0x00401300")
    registration=bytearray()
    branches=[]
    for pointer,previous in ((0x401080,0),(0x401090,0x401080)):
        registration.extend(b"\x68"+struct.pack("<I",pointer)+bytes.fromhex("ff15581140003d")+struct.pack("<I",previous)+b"\x0f\x85")
        branches.append(len(registration));registration.extend(bytes(4))
    registration.extend(bytes.fromhex("6a00ff1558114000c3"))
    failure=len(registration);registration.extend(bytes.fromhex("b8ad0b0000c3"))
    for branch in branches:
        struct.pack_into("<i",registration,branch,failure-branch-4)
    yield "exception-filter", {0:registration,0x80:b"\xc3",0x90:b"\xc3"}, [], False, "SetUnhandledExceptionFilter", "entry_returned", "0x00401090"
    for checked in (True, False):
        code = bytes.fromhex("688c1140006880114000ff155811400083c408")
        if checked:
            code += bytes.fromhex("83f8077506")
        code += bytes.fromhex("a1c0114000c3b8ad0b0000c3")
        body0 = bytes.fromhex("ff05c011400031c0c3")
        body1 = bytes.fromhex("ff05c0114000b807000000c3")
        body2 = bytes.fromhex("cc") if checked else bytes.fromhex("ff05c0114000b80b000000c3")
        patches = {0: code, 0x40: body0, 0x60: body1, 0x80: body2,
                   0x180: struct.pack("<III", 0x401040, 0x401060, 0x401080), 0x1c0: bytes(4)}
        yield ("initterm-e" if checked else "initterm"), patches, [0x401040, 0x401060, 0x401080], True, (
            "_initterm_e" if checked else "_initterm"), "entry_returned", ("0x00000002" if checked else "0x00000003")
    for name, args in (("event-auto", {}), ("event-manual", {"manual":True}),
                       ("event-named", {"named":True}), ("event-blocking", {"blocking":True})):
        patches, imports = event_program(**args)
        yield name, patches, [], False, imports, ("pending_event_wait" if args.get("blocking") else "entry_returned"), (
            None if args.get("blocking") else "0x0000600d")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kit", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--android-clang", type=Path)
    parser.add_argument("--adb")
    parser.add_argument("--serial")
    parser.add_argument("--case", action="append", choices=[case[0] for case in scenarios()])
    args = parser.parse_args()
    if args.adb and not args.android_clang:
        parser.error("--adb requires --android-clang")
    kit = args.kit.resolve()
    profile = json.loads((ROOT / "profiles/startup-initializer.json").read_text())
    results = {}
    result_path = ROOT / ".work/runtime-tests/results.json"
    result_path.parent.mkdir(parents=True, exist_ok=True)
    result_path.write_text("{}\n")
    serial = None
    if args.adb:
        serial = select_device(subprocess.check_output([args.adb, "devices"], text=True), args.serial)
    for name, patches, roots, writable, imported, expected_stop, expected_target in scenarios():
        if args.case and name not in args.case:
            continue
        output = ROOT / ".work/runtime-tests" / ("loop call" if name=="loop-call" else name)
        output.mkdir(parents=True, exist_ok=True)
        data = bytearray(fixture())
        dll_name="KERNEL32.dll"
        if isinstance(imported,dict):dll_name,imported=imported["dll"],imported["names"]
        if isinstance(imported,list):
            data.extend(bytes(0x1200-len(data)))
            struct.pack_into("<I",data,0x178+8,0x1000)
            struct.pack_into("<I",data,0x178+16,0x1000)
            struct.pack_into("<II",data,0x98+96+8,0x1800,40)
            struct.pack_into("<IIIII",data,0xa00,0x1840,0,0,0x1a00,0x1880)
            dll_bytes=dll_name.encode("ascii")+b"\0";assert len(dll_bytes)<64
            data[0xc00:0xc00+len(dll_bytes)]=dll_bytes
            pool=0x18c0
            for index, symbol in enumerate(imported):
                struct.pack_into("<I",data,0xa40+4*index,pool)
                struct.pack_into("<I",data,0xa80+4*index,pool)
                encoded=b"\0\0"+symbol.encode()+b"\0"
                data[pool-0x1000+0x200:pool-0x1000+0x200+len(encoded)]=encoded
                pool+=len(encoded)
            assert pool<0x1a00
        for offset, patch in patches.items():
            data[0x200+offset:0x200+offset+len(patch)] = patch
        if writable:
            struct.pack_into("<I", data, 0x178+36, 0xe0000020)
        if isinstance(imported,str):
            data[0x392:0x3d2] = imported.encode().ljust(64, b"\0")
        source = output / "synthetic.exe"
        source.write_bytes(data)
        profile["source_sha256"] = hashlib.sha256(data).hexdigest()
        prepared = generate(source, kit, output, roots, 256, profile)
        if prepared["translation_errors"] or prepared["frontier"]:
            raise ValueError(f"Synthetic closure incomplete: {name}")
        freeze_source(output)
        target = ["--target=x86_64-w64-windows-gnu"] if os.name == "nt" else []
        executable = build(args.clang.resolve(), target, kit, output)
        content_args=[]
        if name.startswith("content-file-"):
            (output/"fixture.conf").write_bytes(b"A\r\nB\x1aC")
            content_args=[str(output)]
        environment=(os.environ | {"TZ":"UTC0"}) if name.startswith("calendar-") else None
        observed = run_stop([str(executable), str(output / "image.bin"),*content_args], output / "host.log",env=environment)
        assert observed["stop"]==expected_stop and (expected_target is None or observed["target"]==expected_target), (name, observed)
        if name.startswith("filesystem-export-") or name in ("dynamic-export-unknown","dynamic-export-wrong-module"):
            assert observed["stubs"]["optional_filesystem_exports_absent"]==int(name.startswith("filesystem-export-")),observed
        if name in ("initterm", "initterm-e"):
            assert (observed["c_initializers_completed"],observed["cpp_initializers_completed"])==((2,0) if name=="initterm-e" else (0,3))
        results[name] = {"host": observed}
        if os.name=="nt" and name=="basic-ios-normal":
            reference_dll=Path(os.environ.get("WINDIR",r"C:\Windows"))/"SysWOW64/msvcp140.dll"
            expected="f0cda2a0cf1fe6fbbf579b9098462329d3aaa7513a207af2e0f33b01456e388a"
            if hashlib.sha256(reference_dll.read_bytes()).hexdigest()!=expected:raise ValueError("MSVCP reference DLL changed")
            oracle=output/"original-msvcp.exe"
            subprocess.run([str(args.clang.resolve()),"--target=i686-w64-windows-gnu","-O1","-Wall","-Wextra","-Werror",
                            str(ROOT/"tests/x86_msvcp_ios_oracle_win.c"),"-o",str(oracle)],check=True)
            reference=json.loads(subprocess.check_output([str(oracle),str(reference_dll)],text=True,timeout=30))
            assert reference==dict(return_this=True,fields_match=True,vftable_rva=0x2f64),reference
            results[name]["original_x86_msvcp"]=reference
        if os.name=="nt" and name.startswith(("simd-paddd","simd-scalar-double")):
            oracle=output/"original-x86.exe"
            subprocess.run([str(args.clang.resolve()),"--target=i686-w64-windows-gnu","-O1","-Wall","-Wextra","-Werror",
                            str(ROOT/"tests/x86_simd_oracle_win.c"),"-Wl,--image-base,0x30000000",
                            "-Wl,--disable-dynamicbase","-o",str(oracle)],check=True)
            decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32);decoder.detail=True
            relocations=[]
            for instruction in decoder.disasm(bytes(patches[0]),0x401000):
                if any(op.type==X86_OP_MEM and not op.mem.base and not op.mem.index
                       and 0x400000<=op.mem.disp<0x403000 for op in instruction.operands):
                    assert instruction.disp_size==4
                    relocations.append(instruction.address-0x400000+instruction.disp_offset)
                if (not instruction.group(capstone.CS_GRP_CALL) and not instruction.group(capstone.CS_GRP_JUMP)
                        and any(op.type==X86_OP_IMM and 0x400000<=op.imm<0x403000 for op in instruction.operands)):
                    assert instruction.imm_size==4
                    relocations.append(instruction.address-0x400000+instruction.imm_offset)
            relocation_file=output/"oracle-relocations.bin"
            relocation_file.write_bytes(struct.pack("<"+"I"*len(relocations),*relocations))
            reference=json.loads(subprocess.check_output([str(oracle),str(output/"image.bin"),str(relocation_file)],text=True,timeout=30))
            assert reference==({"return":0x600d} if expected_stop=="entry_returned" else {"fault":"0xc0000005"}),reference
            results[name]["original_x86"]=reference
        if serial:
            android = build(args.android_clang.resolve(), [], kit, output, android=True)
            remote = "/data/local/tmp/chusan-synthetic-" + uuid.uuid4().hex[:12]
            command = [args.adb, "-s", serial]
            def adb(*parts, check=True):
                return subprocess.run([*command, *parts], check=check, capture_output=True, text=True, timeout=30)
            try:
                adb("shell", "mkdir", remote)
                adb("push", str(android), remote + "/runtime")
                adb("push", str(output / "image.bin"), remote + "/image.bin")
                if content_args:adb("push",str(output/"fixture.conf"),remote+"/fixture.conf")
                adb("shell", "chmod", "700", remote + "/runtime")
                timezone_args=["env","TZ=UTC0"] if environment else []
                observed = run_stop([*command, "shell",*timezone_args,remote + "/runtime", remote + "/image.bin",
                                     *([remote] if content_args else [])], output / "android.log")
                assert observed["stop"]==expected_stop and (expected_target is None or observed["target"]==expected_target), (name, observed)
                if name.startswith("filesystem-export-") or name in ("dynamic-export-unknown","dynamic-export-wrong-module"):
                    assert observed["stubs"]["optional_filesystem_exports_absent"]==int(name.startswith("filesystem-export-")),observed
                if name in ("initterm", "initterm-e"):
                    assert (observed["c_initializers_completed"],observed["cpp_initializers_completed"])==((2,0) if name=="initterm-e" else (0,3))
                results[name]["android"] = observed
            finally:
                adb("shell", "rm", "-f", remote + "/runtime", remote + "/image.bin", check=False)
                if content_args:adb("shell","rm","-f",remote+"/fixture.conf",check=False)
                adb("shell", "rmdir", remote, check=False)
        result_path.write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps({"scenarios_passed": len(results), "platforms": ["host", "android"] if serial else ["host"]}))


if __name__ == "__main__":
    main()
