# SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
# Copyright 2020 - 2025 copy  Freescale Semiconductor Inc

# ==============================================================================
## @file            trace.tcl
## @brief           CPE trace TCL interface.
## @authors         NXP Semiconductors.
# ==============================================================================

namespace eval trace {
    namespace export dump
}

namespace eval const::trace {
    variable L1_TRACE_SIZE 4*192

    array set TRACE_MSG {
        0x000 L1_TRACE_EMPTY

    0x100  L1_TRACE_MSG_DMA_XFR = L1_TRACE_MSG_DMA
    0x101  L1_TRACE_MSG_DMA_CFGERR
    0x102  L1_TRACE_MSG_DMA_AXIQ_RX
    0x103  L1_TRACE_MSG_DMA_AXIQ_TX
    0x104  L1_TRACE_MSG_DMA_AXIQ_RX_ERR
    0x105  L1_TRACE_MSG_DMA_AXIQ_TX_ERR
    0x106  L1_TRACE_MSG_DMA_AXIQ_TX_CTRL
    0x107  L1_TRACE_MSG_DMA_AXIQ_DISABLE
    0x108  L1_TRACE_MSG_DMA_AXIQ_SIZE
    0x109  L1_TRACE_MSG_DMA_XFR_SRC 
    0x10A  L1_TRACE_MSG_DMA_XFR_SIZE 
    0x10B  L1_TRACE_MSG_DMA_XFR_DST 
    0x10C  L1_TRACE_MSG_DMA_AXIQ_RESET 
    0x10D  L1_TRACE_MSG_DMA_XFR_UNDERRUN
                
	 0x200 L1_TRACE_MSG_L1C_SSB_SCAN
	 0x201 L1_TRACE_MSG_L1C_SSB_STOP
	 0x202 L1_TRACE_MSG_L1C_DL_MSG
	 0x203 L1_TRACE_MSG_L1C_DL_SLOTCONFIG
	 0x204 L1_TRACE_MSG_L1C_DL_SEMISTATIC
	 0x205 L1_TRACE_MSG_L1C_DL_SLOTUPDATE
	 0x206 L1_TRACE_MSG_L1C_UL_START
	 0x207 L1_TRACE_MSG_L1C_UL_MSG
	 0x208 L1_TRACE_MSG_L1C_DL_CONSUME
	 0x209 L1_TRACE_MSG_L1C_UL_CONSUME
     0x20A L1_TRACE_MSG_L1C_FREE
    
	 0x300 L1_TRACE_L1APP_FETCH_NEXT_TC
	 0x301 L1_TRACE_L1APP_SLOT_TRANSITION_TX_C
	 0x302 L1_TRACE_L1APP_UPSAMPLE_BY_4_A
	 0x303 L1_TRACE_L1APP_UPSAMPLE_BY_4_B
	 0x304 L1_TRACE_L1APP_BUFF_ID
	 0x305 L1_TRACE_L1APP_BUFF_ID_HS
	 0x306 L1_TRACE_L1APP_SLOT_ID
     0x307 L1_TRACE_L1APP_SYM_ID    
     0x308 L1_TRACE_L1APP_LAST_SYM    
     0x309 L1_TRACE_L1APP_BUFF_ID_OUT
	 
	 0xA00 L1_TRACE_MSG_OVLY_SSB
	 0xA01 L1_TRACE_MSG_OVLY_DL
	 0xA02 L1_TRACE_MSG_OVLY_UL
	 0xA03 L1_TRACE_MSG_OVLY_ERR
	 0xA04 L1_TRACE_MSG_OVLY_DL_DYNAMIC
	 
	 0xB00 L1_TRACE_MSG_IPPU_ERROR
	 
	 0xC00 L1_TRACE_MSG_ENTRY_IDLE
	 0xC01 L1_TRACE_MSG_ENTRY_MAIN
	 0xC02 L1_TRACE_MSG_ENTRY_SSB
	 0xC03 L1_TRACE_MSG_ENTRY_DL_A
	 0xC04 L1_TRACE_MSG_ENTRY_DL_B
	 0xC05 L1_TRACE_MSG_ENTRY_DL_C
	 0xC06 L1_TRACE_MSG_ENTRY_UL_A
	 0xC07 L1_TRACE_MSG_ENTRY_UL_B
	 0xC08 L1_TRACE_MSG_ENTRY_UL_C
	 
	 0xD00 L1_TRACE_MSG_EXCEPTION
	 0xD01 L1_TRACE_MSG_AXIS_DLA2DLB
	 0xD02 L1_TRACE_MSG_AXIS_DLB2DLC_DLC
	 0xD03 L1_TRACE_MSG_AXIS_DLA2DLB_SSB240
	 0xD04 L1_TRACE_MSG_DL_B2A_UL_MSG
    }
}

proc trace::get_msg_name_from_val {val} {
    if { [catch {set msg_name $::const::trace::TRACE_MSG(0x[format %03X $val])}] } {
        set msg_name [format 0x%08X $val]
    }
    return $msg_name
}

proc trace::parse_record {counter msg param} {
    array set record {}
    set record(counter) [format %016lu $counter]
    set record(msg)   [trace::get_msg_name_from_val $msg]
    set record(param) [format 0x%08X $param]
    return [array get record]
}

proc trace::parse {} {
    stop
    set trace_addr [evaluate #x l1_trace_data]
    vspa::save $trace_addr tracedump.tmp $::const::trace::L1_TRACE_SIZE*4
    set fd [open tracedump.tmp]
    set file_lines [split [read $fd] "\n"]
    close $fd
    file delete tracedump.tmp
    set traces {}
    foreach {w0 w1 w2 w3} [lrange $file_lines 0 end-1] {
        lappend traces [parse_record 0x$w1$w0 0x$w2 0x$w3]
    }
    return $traces
}

proc trace::print {traces {descriptor "stdout"}} {
    config hexPrefix 0x
    set index 0
    foreach trace $traces {
        array set record $trace
        if {$record(msg) != "L1_TRACE_EMPTY"} {
            puts $descriptor "[format %03s $index], counter: $record(counter), param: $record(param), $record(msg)"
            incr index
        }
    }
    puts $descriptor "l1_trace_index: [format %04u [var v:"l1-trace.c"::l1_trace_index -np]]"
}

proc trace::dump_to_file {filename traces} {
    set fd [open $filename w]
    trace::print $traces $fd
    close $fd
}

proc trace::dump { {filename ""}} {
    set traces [trace::parse ]
    if {$filename == ""} {
        trace::print $traces
    } else {
        trace::dump_to_file $filename $traces
    }
}