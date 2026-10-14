Attribute VB_Name = "modSound"
Option Explicit

Private Declare Function waveOutOpen Lib "winmm.dll" ( _
    ByRef lphWaveOut As Long, ByVal uDeviceID As Long, _
    ByRef lpFormat As WAVEFORMATEX, ByVal dwCallback As Long, _
    ByVal dwInstance As Long, ByVal dwFlags As Long) As Long

Private Declare Function waveOutPrepareHeader Lib "winmm.dll" ( _
    ByVal hWaveOut As Long, ByRef lpWaveOutHdr As WAVEHDR, _
    ByVal uSize As Long) As Long

Private Declare Function waveOutWrite Lib "winmm.dll" ( _
    ByVal hWaveOut As Long, ByRef lpWaveOutHdr As WAVEHDR, _
    ByVal uSize As Long) As Long

Private Declare Function waveOutUnprepareHeader Lib "winmm.dll" ( _
    ByVal hWaveOut As Long, ByRef lpWaveOutHdr As WAVEHDR, _
    ByVal uSize As Long) As Long

Private Declare Function waveOutReset Lib "winmm.dll" ( _
    ByVal hWaveOut As Long) As Long

Private Declare Function waveOutClose Lib "winmm.dll" ( _
    ByVal hWaveOut As Long) As Long

Private Declare Sub ZeroMemory Lib "kernel32.dll" Alias "RtlZeroMemory" ( _
    ByVal Destination As Long, ByVal Length As Long)

Private Declare Sub CopyMemory Lib "kernel32.dll" Alias "RtlMoveMemory" ( _
    ByRef Destination As Any, ByRef Source As Any, ByVal Length As Long)

Private Type WAVEFORMATEX
    wFormatTag As Integer
    nChannels As Integer
    nSamplesPerSec As Long
    nAvgBytesPerSec As Long
    nBlockAlign As Integer
    wBitsPerSample As Integer
    cbSize As Integer
End Type

Private Type WAVEHDR
    lpData As Long
    dwBufferLength As Long
    dwBytesRecorded As Long
    dwUser As Long
    dwFlags As Long
    dwLoops As Long
    lpNext As Long
    reserved As Long
End Type

Private Const WAVE_FORMAT_PCM As Long = 1
Private Const WAVE_MAPPER As Long = -1
Private Const WHDR_DONE As Long = &H1
Private Const WHDR_PREPARED As Long = &H2

Public Const SND_SAMPLE_RATE As Long = 32768
Public Const SND_CHANNELS As Long = 2
Public Const SND_BITS As Long = 16

' 1프레임: 548샘플 * 4바이트 = 2192 바이트 (int16 기준 1096개)
Public Const SND_BUFFER_SIZE As Long = 2192
Private Const NUM_BUFFERS As Long = 6

' [중요!] VB6 2차원 배열의 Column-Major 메모리 꼬임을 방지하기 위한 1차원 래퍼 구조체
Private Type SoundBuffer
    Data(0 To SND_BUFFER_SIZE - 1) As Byte
End Type

Public Declare Function GBA_GetAudioSamples Lib "gdkgba.dll" ( _
    ByVal pBuffer As Long, ByVal nByteLen As Long) As Long

Private m_hWaveOut As Long
Private m_Headers(0 To NUM_BUFFERS - 1) As WAVEHDR
Private m_Buffers(0 To NUM_BUFFERS - 1) As SoundBuffer
Private m_CurrentBufIdx As Long
Private m_LastWrittenIdx As Long
Private m_bSoundInitialized As Boolean

Public Function Sound_Init() As Boolean
    Dim wfx As WAVEFORMATEX
    Dim i As Long
    Dim ret As Long

    If m_bSoundInitialized Then Call Sound_Close

    With wfx
        .wFormatTag = WAVE_FORMAT_PCM
        .nChannels = SND_CHANNELS
        .nSamplesPerSec = SND_SAMPLE_RATE
        .wBitsPerSample = SND_BITS
        .nBlockAlign = 4
        .nAvgBytesPerSec = SND_SAMPLE_RATE * 4
        .cbSize = 0
    End With

    ret = waveOutOpen(m_hWaveOut, WAVE_MAPPER, wfx, 0, 0, 0)
    If ret <> 0 Then
        Sound_Init = False
        Exit Function
    End If

    For i = 0 To NUM_BUFFERS - 1
        Call ZeroMemory(VarPtr(m_Buffers(i).Data(0)), SND_BUFFER_SIZE)
        With m_Headers(i)
            .lpData = VarPtr(m_Buffers(i).Data(0))
            .dwBufferLength = SND_BUFFER_SIZE
            .dwFlags = 0
            .dwLoops = 0
        End With
        Call waveOutPrepareHeader(m_hWaveOut, m_Headers(i), Len(m_Headers(i)))
        
        ' 처음엔 모든 버퍼를 WHDR_DONE 상태로 표시해 두어 즉시 사용할 수 있게 함
        m_Headers(i).dwFlags = m_Headers(i).dwFlags Or WHDR_DONE
    Next i

    ' 초기 2개 버퍼만 무음으로 먼저 밀어 넣어 재생 큐 시작
    For i = 0 To 1
        m_Headers(i).dwFlags = m_Headers(i).dwFlags And Not WHDR_DONE
        Call waveOutWrite(m_hWaveOut, m_Headers(i), Len(m_Headers(i)))
    Next i

    m_CurrentBufIdx = 2
    m_LastWrittenIdx = 0
    m_bSoundInitialized = True
    Sound_Init = True
End Function

Public Sub Sound_Update()
    If Not m_bSoundInitialized Then Exit Sub

    ' 현재 순서의 버퍼가 재생 완료(WHDR_DONE)되었을 때만 안전하게 새 샘플을 채워 넣음
    If (m_Headers(m_CurrentBufIdx).dwFlags And WHDR_DONE) = WHDR_DONE Then
        
        ' 1. C DLL에서 연속된 1차원 버퍼 메모리로 정확히 2192바이트 복사
        Call GBA_GetAudioSamples(VarPtr(m_Buffers(m_CurrentBufIdx).Data(0)), SND_BUFFER_SIZE)

        ' 2. DONE 플래그만 끄고(WHDR_PREPARED 유지) 사운드 카드에 전송
        m_Headers(m_CurrentBufIdx).dwFlags = m_Headers(m_CurrentBufIdx).dwFlags And Not WHDR_DONE
        Call waveOutWrite(m_hWaveOut, m_Headers(m_CurrentBufIdx), Len(m_Headers(m_CurrentBufIdx)))

        ' 3. 디버그용으로 방금 쓴 버퍼 인덱스 기록 후 다음 인덱스로 이동
        m_LastWrittenIdx = m_CurrentBufIdx
        m_CurrentBufIdx = (m_CurrentBufIdx + 1) Mod NUM_BUFFERS
    End If
End Sub

Public Sub Sound_Close()
    Dim i As Long

    If Not m_bSoundInitialized Then Exit Sub
    m_bSoundInitialized = False

    If m_hWaveOut <> 0 Then
        Call waveOutReset(m_hWaveOut)
        For i = 0 To NUM_BUFFERS - 1
            Call waveOutUnprepareHeader(m_hWaveOut, m_Headers(i), Len(m_Headers(i)))
        Next i
        Call waveOutClose(m_hWaveOut)
        m_hWaveOut = 0
    End If
End Sub

Public Function Sound_GetDebugInfo() As String
    If Not m_bSoundInitialized Then
        Sound_GetDebugInfo = "Sound: Not Init"
        Exit Function
    End If

    Dim i As Long
    Dim sampleVal As Integer
    Dim maxAmp As Long
    Dim nonZeroCount As Long
    Dim totalSamples As Long

    maxAmp = 0
    nonZeroCount = 0
    totalSamples = SND_BUFFER_SIZE \ 2 ' 1096개

    ' 방금 실제로 기록한 버퍼(m_LastWrittenIdx)를 검사하도록 수정!
    For i = 0 To (SND_BUFFER_SIZE - 2) Step 2
        Call CopyMemory(sampleVal, m_Buffers(m_LastWrittenIdx).Data(i), 2)
        If sampleVal <> 0 Then
            nonZeroCount = nonZeroCount + 1
            If Abs(CLng(sampleVal)) > maxAmp Then
                maxAmp = Abs(CLng(sampleVal))
            End If
        End If
    Next i

    Sound_GetDebugInfo = "Buf: " & m_LastWrittenIdx & _
                         " | NextDone: " & ((m_Headers(m_CurrentBufIdx).dwFlags And WHDR_DONE) = WHDR_DONE) & _
                         " | NonZero: " & nonZeroCount & "/" & totalSamples & _
                         " | MaxAmp: " & maxAmp
End Function

