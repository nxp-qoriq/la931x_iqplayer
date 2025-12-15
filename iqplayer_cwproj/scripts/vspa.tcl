# SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
# Copyright 2017 - 2025 copy  Freescale Semiconductor Inc

##
# @file         vspa.tcl
# @brief        VSPA interface.
# @author       NXP Semiconductors.

namespace eval vspa {
    set version 1.0

    namespace export peek
    namespace export poke
    namespace export load
    namespace export save

    namespace export addr

    namespace export is_stopped

    namespace export is_running

    namespace eval swap {
        namespace export 16
        namespace export 32
        namespace export 64
    }

    namespace eval q15 {
        namespace export enc
        namespace export dec
    }

    namespace eval fixed16 {
        namespace export enc
        namespace export dec
    }

    namespace eval float16 {
        namespace export enc
        namespace export dec
    }

    namespace eval float32 {
        namespace export enc
        namespace export dec
    }

    namespace eval float64 {
        namespace export enc
        namespace export dec
    }

    namespace eval mbox {
        namespace export is_valid
        namespace export is_full
        namespace export rd32
        namespace export rd64
        namespace export wr32
        namespace export wr64
    }

    namespace export freq2nco
    namespace export nco2freq
}

package provide vspa $vspa::version

##
# @brief    32-bit DMEM read access
#
# This function reads @c size 32-bit word(s) starting at address @c addr.
# 
# @param    addr Specifies the word-aligned address to read from.
# @param    size Specifies the number of 32-bit words to read.
#                Defaults to 1.
#
# @return   The 32-bit word(s) read from DMEM address space.
#
proc vspa::peek {addr {size 1}} {
    set data [mem m:[format "0x%8.8X" \
        [expr $addr]]..[format "0x%8.8X" [expr $addr+2*$size-1]] -s -np]
    return $data
}

##
# @brief    32-bit DMEM write access
#
# This procedure writes @c size 32-bit word(s) starting at address @c addr.
#
# @param    addr Specifies the word-aligned address to write to.
# @param    data Value to write.
# @param    size Specifies the number of 32-bit words to write.
#                Defaults to 1.
#
# @return   void.
#
proc vspa::poke {addr data {size 1}} {
    mem m:[format "0x%8.8X" [expr $addr]]..[format "0x%8.8X" \
        [expr $addr+2*$size-1]] -s =[format "0x%8.8X" [expr $data]]
}

##
# @brief    32-bit Physical MEM write access
#
# This procedure writes @c size 32-bit word(s) starting at address @c addr.
#
# @param    addr Specifies the word-aligned address to write to.
# @param    data Value to write.
# @param    size Specifies the number of 32-bit words to write.
#                Defaults to 1.
#
# @return   void.
#
proc vspa::ppoke {addr data {size 1}} {
    mem p:[format "0x%8.8X" [expr $addr]]..[format "0x%8.8X" \
        [expr $addr+2*$size-1]] -s =[format "0x%8.8X" [expr $data]]
}

##
# @brief    Fill a portion of the DMEM address space
#
# This procedure writes the value(s) in @c data to the array starting at @c addr.
#
# If @c size is omitted or zero, the number of elements in @c data specifies the
# size of the written array.
#
# If @c size is not zero and less than the size of @c data, then @c size specifies
# the size of the written array, and the array is written with the first @c size
# elements from @c data.
#
# Otherwise, only the nunmber of elements in @c data are written.
#
# @param    addr Specifies the word-aligned address to write to.
# @param    data List of data to write.
# @param    size Specifies the number of 32-bit words to write.
#                Defaults to 0.
#
# @return   void.
#
proc vspa::fill {addr data {size 0}} {
    if {([expr $size] > 0) && ([expr $size] < [llength $data])} {
        set data [lrange $data 0 [expr 2*$size-1]]
    }
    foreach val $data {
        mem m:[format "0x%8.8X" [expr $addr]] -s =$val
        incr addr
    }
}

##
# @brief    Save a portion of the DMEM address space to a file
#
# This procedure saves @c size 32-bit words starting at address @c addr
# to the file @c path.
#
# @param    addr Specifies the word-aligned address to read from.
# @param    path Specifies the relative or absolute path for the output file.
# @param    size Specifies the number of 32-bit words to save (defaults to 1).
#
# @return   void.
#
proc vspa::save {addr path {size 1}} {
    set pbin [format "%s.bin" [file rootname $path]]
    file delete $pbin
    if {[catch {::save -b m:[format "0x%8.8X" \
         [expr $addr]]..[format "0x%8.8X" [expr $addr+2*$size-1]] $pbin -o} msg]} {
        error $msg
    }
    if {[catch {set fbin [open $pbin {RDONLY}]} msg]} {
        error $msg
    }
    fconfigure $fbin -translation binary
    set dbin [read $fbin]
    close $fbin
    file delete $pbin
    binary scan $dbin i* dhex
    file delete $path
    if {[catch {set fhex [open $path {WRONLY CREAT}]} msg]} {
        error $msg
    }
    foreach val $dhex {
        puts $fhex [format "%08X" $val]
    }
    close $fhex
}

##
# @brief    Load a portion of the DMEM address space from a file
#
# This procedure writes an array of 32-bit elements starting at address @c addr.
#
# If @c size is omitted or zero, the size of the array is defined by the number
# of 32-bit elements in the file @c path.
#
# If @c size is greater than zero, and less than the number of 32-bit elements
# in the file @c path, the size of the array is @c size.
#
# If @c size is greater than zero, and greater than the number of 32-bit elements
# in the file @c path, the size of the array is the number of 32-bit elements
# in the file @c path.
#
# @param    addr Specifies the word-aligned address to read from.
# @param    path Specifies the relative or absolute path for the input file.
# @param    size Specifies the number of 32-bit words to load (defaults to 0).
#
# @return   void.
#
proc vspa::load {addr path {size 0}} {
    if {![file isfile $path]} {
        error "$path is not a file."
    }
    if {[catch {set fhex [open $path {RDONLY}]} msg]} {
        error $msg
    }
    set dhex [read -nonewline $fhex]
    close $fhex

    set pbin [format "%s.bin" [file rootname $path]]
    if {[catch {set fbin [open $pbin {WRONLY CREAT}]} msg]} {
        error $msg
    }
    fconfigure $fbin -translation binary
    if {($size > 0) && ([expr $size] < [llength $dhex])} {
        set dhex [lrange $dhex 0 [expr $size-1]]
    }
    foreach val $dhex {
        puts -nonewline $fbin [binary format i* [scan $val "%08x\n"]]
    }
    close $fbin
    if {[catch {restore -b $pbin m:[format "0x%8.8X" [expr $addr]]} msg]} {
        error $msg
    }
    file delete $pbin
}

##
# @brief        Get the address of a variable
# @param        name    The name of the variable.
# @return       The address of the variable @a name in hexadecimal format.
#
proc vspa::addr {name} {
    return [format 0x%4.4X [var &${name}\[0\] -np %X]]
}

##
# @brief        Check if cores are stopped.
#
# @param        cores   Single index or list of cores indexes.
# @retval       zero    At least one core is not stopped.
# @retval       non-zero All cores are stopped.
#
# @todo         Make this function more robust by linking index to PID.
proc vspa::is_stopped {cores} {
    set mcstat [status]
    set mcstat [split $mcstat "\n"]
    set stopped 1
    foreach core $cores {
        set corestat [lindex $mcstat $core]
        regexp {(Stopped|Running)} $corestat state
        if {[string compare -nocase "stopped" $state] != 0} {
            set stopped 0
        }
    }
    return $stopped;
}

##
# @brief        Check if cores are running.
#
# @param        cores   Single index or list of cores indexes.
# @retval       zero    At least one core is not running.
# @retval       non-zero All cores are running.
#
proc vspa::is_running {cores} {
    set mcstat [status]
    set mcstat [split $mcstat "\n"]
    set running 1
    foreach core $cores {
        set corestat [lindex $mcstat $core]
        regexp {(Stopped|Running)} $corestat state
        if {[string compare -nocase "running" $state] != 0} {
            set running 0
        }
    }
    return $running;
}

##
# @brief    Change a 16-bit data endianness
#
# This function swaps the 2 bytes in a 16-bit word.
#
# @param    x The 16-bit data.
#
# @return   The endianness-modified 16-bit data.
#
proc vspa::swap::16 {x} {
    set b0 0
    set b1 0
    scan [format "%4.4x" [expr $x]] "%2x%2x" b1 b0
    return [format "%02X%02X" $b0 $b1]
}

##
# @brief    Change a 32-bit data endianness
#
# This function swaps the 4 bytes in a 32-bit word.
#
# @param    x The 32-bit data.
#
# @return   The endianness-modified 32-bit data.
#
proc vspa::swap::32 {x} {
    set b0 0
    set b1 0
    set b2 0
    set b3 0
    scan [format "%8.8x" [expr $x]] "%2x%2x%2x%2x" b3 b2 b1 b0
    return [format "%02X%02X%02X%02X" $b0 $b1 $b2 $b3]
}

##
# @brief    Change a 64-bit data endianness
#
# This function swaps the 8 bytes in a 64-bit word.
#
# @param    x The 64-bit data.
#
# @return   The endianness-modified 64-bit data.
#
proc vspa::swap::64 {x} {
    set b0 0
    set b1 0
    set b2 0
    set b3 0
    set b4 0
    set b5 0
    set b6 0
    set b7 0
    scan [format "%16.16lx" [expr wide( $x )]] "%2x%2x%2x%2x%2x%2x%2x%2x" \
        b7 b6 b5 b4 b3 b2 b1 b0
    return [format "%02X%02X%02X%02X%02X%02X%02X%02X" \
        $b0 $b1 $b2 $b3 $b4 $b5 $b6 $b7]
}

proc vspa::q15::enc {x} {

}

proc vspa::q15::dec {x} {

}

##
# @brief    Encode a floating point value to 16-bit fixed-point format.
#
# This function encodes a floating point value in the range ]-1.0, +1.0[
# to VSPA fixed-point format.
#
# @param    x The floating point value to encode.
#
# @return   The 16-bit fixed-point value corresponding to @c x.
#
proc vspa::fixed16::enc {x} {
    if { [expr abs( $x )] > 1 } {
        error "VSPA: cannot convert $x to fixed16 \
            (absolute value must be less than 1)!"
    }
    set x16 [expr int( abs( $x ) * pow(2, 15) )]
    if { $x16 > 32767 } {
        set x16 32767
    }
    if { $x < 0 } {
        incr x16 32768
    }
    return [format "0x%04X" $x16]
}

##
# @brief    Decode a 16-bit fixed-point number to a floating point value.
#
# This function deccodes a VSP fixed-point number to a floating point value
# in the range ]-1.0, +1.0[.
#
# @param    x The 16-bit fixed-point number to decode.
#
# @return   The floating point value corresponding to @c x.
#
proc vspa::fixed16::dec {x} {

    # Get the sign:
    if { [expr ($x) & 0x8000] } {
        set s -1
    } else {
        set s 1
    }

    # Get the magnitude:
    set m [expr $x & 0x7FFF]

    # Format the output value:
    return [expr $s * $m / pow( 2, 15 )]
}

proc vspa::float16::enc {x} {

}

proc vspa::float16::dec {x} {

}

##
# @brief    Encode a floating point value to 32-bit single precision format.
#
# This function encodes a floating point value to VSP single precision format.
#
# @param    x The floating point value to encode.
#
# @return   The 32-bit single precision value in hexadecimal format.
#
proc vspa::float32::enc {x} {
    set float [expr $x]
    binary scan [binary format f* $float] i hex
    return [format "0x%8.8X" $hex]
}

##
# @brief    Decode a 32-bit single precision number to a floating point value.
#
# This function decodes a VSP single precision value to a floating point value.
#
# @param    x The 32-bit single precision number to decode.
#
# @return   The floating point value corresponding to @c x.
#
proc vspa::float32::dec {x} {
    set be [vspa::swap::32 [expr $x]]
    set bin [binary format H* $be]
    binary scan $bin f* float
    return $float
}

##
# @brief    Encode a floating point value to 64-bit double precision format.
#
# This function encodes a floating point value to VSP double precision format.
#
# @param    x The floating point value to encode.
#
# @return   The 64-bit double precision value in hexadecimal format.
#
proc vspa::float64::enc {x} {
    binary scan [binary format d [expr double( $x )]] w hex
    return [format "0x%16.16lX" $hex]
}

##
# @brief    Decode a 64-bit single precision number to a floating point value.
#
# This function decodes a VSP double precision value to a floating point value.
#
# @param    x The 64-bit single precision number to decode.
#
# @return   The floating point value corresponding to @c x.
#
proc vspa::float64::dec {x} {
    set be [vspa::swap::64 [expr wide( $x )]]
    set bin [binary format H* $be]
    binary scan $bin d* float
    return [expr double( $float )]
}

proc vspa::mbox::is_valid {} {
    set status [format "0x%8.8X" [expr [mem ip:0x1A8 -s -np] & (0x1 << 2)]]
    return $status
}

proc vspa::mbox::is_full {} {
    set status [format "0x%8.8X" [expr [mem ip:0x1A8 -s -np] & (0x1 << 0)]]
    return $status
}

proc vspa::mbox::rd32 {} {
    set msg [format "0x%8.8X" [mem ip:0x1A5 -s -np]]
    return $msg
}

proc vspa::mbox::rd64 {} {
    set msb [format "0x%8.8X" [mem ip:0x1A4 -s -np]]
    set lsb [format "0x%8.8X" [mem ip:0x1A5 -s -np]]
    return [format "0x%16.16lX" [expr (wide($msb) << 32) | wide($lsb)]]
}

proc vspa::mbox::wr32 {msg} {
    mem ip:0x1A1 -s =$msg
}

proc vspa::mbox::wr64 {msg} {
    mem ip:0x1A0 -s =[format "0x%8.8X" [expr wide($msg) >> 32]]
    mem ip:0x1A1 -s =[format "0x%8.8X" [expr wide($msg) & wide(0x00000000FFFFFFFF)]]
}

proc vspa::freq2nco {f} {
    if {$f > 0} {
        set nco [expr round($f * pow(2.0, 32))]
    } else {
        set nco [expr wide(pow(2.0, 32)-1) ^ abs(round($f * pow(2.0, 32)))]
    }
    return $nco
}

proc vspa::nco2freq {nco} {
    if {$nco < [expr wide(pow(2, 31))]} {
        set f [expr double($nco) / wide(pow(2, 32))]
    } else {
        set f [expr (-1) * double(wide($nco) ^ wide(pow(2.0, 32)-1)) / pow(2, 32)]
    }
    return $f
}

proc vspa::boot {target} {
    switchtarget ${target}
    while {[expr [reg HOST_MBOX_STATUS -np] & 4] == 0} {
        wait 500
    }
    set lsb [reg HOST_IN_0_LSB -np]
    set msb [reg HOST_IN_0_MSB -np]
    puts "\[VSPA${target}\] -> msg [format 0x%8.8X%8.8X $msb $lsb]"
    puts "\[VSPA${target}\] <- msg 0x7000000000000000"
    reg HOST_OUT_0_MSB=0x70000000
    reg HOST_OUT_0_LSB=0
    while {[expr [reg HOST_MBOX_STATUS -np] & 4] == 0} {
        wait 500
    }
    set lsb [reg HOST_IN_0_LSB -np]
    set msb [reg HOST_IN_0_MSB -np]
    puts "\[VSPA${target}\] -> msg [format 0x%8.8X%8.8X $msb $lsb]"
}
