import numpy as np
import pandas as pd
import librosa
import matplotlib.pyplot as plt

from scipy.signal import (
    hilbert,
    butter,
    filtfilt,
    find_peaks,
    stft
)

from sklearn.cluster import KMeans


# ============================
# CONFIGURATION
# ============================

AUDIO_FILE = "morse_stego.wav"

ENVELOPE_CUTOFF = 20       # Hz
MIN_TONE_LENGTH = 0.005    # seconds

BIT_THRESHOLD_RATIO = 1.25

OUTPUT_CSV = "morse_timings.csv"


# ============================
# LOAD AUDIO
# ============================

def load_audio(filename):

    audio, sr = librosa.load(
        filename,
        sr=None,
        mono=True
    )

    # normalize
    audio = audio / np.max(np.abs(audio))

    print(f"[+] Sample rate: {sr}")
    print(f"[+] Duration: {len(audio)/sr:.2f}s")

    return audio, sr



# ============================
# ENVELOPE EXTRACTION
# ============================

def get_envelope(audio, sr):

    analytic = hilbert(audio)

    envelope = np.abs(analytic)


    # Low-pass filter
    b, a = butter(
        3,
        ENVELOPE_CUTOFF/(sr/2),
        btype="low"
    )

    envelope = filtfilt(
        b,
        a,
        envelope
    )


    envelope /= np.max(envelope)

    return envelope



# ============================
# AUTOMATIC THRESHOLD
# ============================

def detect_threshold(envelope):

    # percentile based noise floor

    noise = np.percentile(
        envelope,
        20
    )

    peak = np.percentile(
        envelope,
        95
    )


    threshold = noise + (
        peak-noise
    )*0.35


    print(
        f"[+] Detection threshold: {threshold:.3f}"
    )

    return threshold



# ============================
# FIND TONE BURSTS
# ============================

def detect_tones(envelope, sr):

    threshold = detect_threshold(
        envelope
    )


    active = envelope > threshold


    changes = np.diff(
        active.astype(int)
    )


    starts = np.where(
        changes == 1
    )[0]

    ends = np.where(
        changes == -1
    )[0]


    if len(ends) < len(starts):
        ends = np.append(
            ends,
            len(envelope)-1
        )


    tones=[]


    for s,e in zip(starts,ends):

        duration=(e-s)/sr

        if duration > MIN_TONE_LENGTH:

            tones.append(
                (
                    s/sr,
                    e/sr,
                    duration
                )
            )


    print(
        f"[+] Tone bursts detected: {len(tones)}"
    )

    return tones



# ============================
# GAP EXTRACTION
# ============================

def extract_gaps(tones):

    gaps=[]

    for i in range(len(tones)-1):

        gap = (
            tones[i+1][0]
            -
            tones[i][1]
        )

        gaps.append(gap)


    return np.array(gaps)



# ============================
# FIND MORSE UNIT TIME
# ============================

def estimate_unit(gaps):

    values=gaps.reshape(-1,1)


    # cluster gap lengths

    kmeans=KMeans(
        n_clusters=3,
        random_state=0,
        n_init=10
    )


    kmeans.fit(values)


    centers=np.sort(
        kmeans.cluster_centers_.flatten()
    )


    print(
        "[+] Gap clusters:",
        centers
    )


    # shortest gap ≈ 1T

    unit=centers[0]


    print(
        f"[+] Estimated Morse unit: {unit:.4f}s"
    )

    return unit



# ============================
# GAP BIT DECODER
# ============================

def decode_gap_bits(gaps, unit):


    bits=[]


    expected=unit


    for g in gaps:


        error=g-expected


        # timing modulation

        if abs(error) > unit*0.25:

            bit=1 if error>0 else 0

        else:

            bit=0


        bits.append(bit)


    return bits



# ============================
# ASCII DECODER
# ============================

def bits_to_ascii(bits):

    output=[]


    for reverse in [False,True]:


        data=bits.copy()


        if reverse:
            data=data[::-1]


        chars=[]


        for i in range(
            0,
            len(data)-7,
            8
        ):

            byte=data[i:i+8]

            value=0


            for b in byte:

                value=(value<<1)|b


            chars.append(
                chr(value)
                if 32 <= value <=126
                else "."
            )


        output.append(
            "".join(chars)
        )


    return output



# ============================
# SUB-AUDIBLE ANALYSIS
# ============================

def analyze_spectrum(audio,sr):

    f,t,Z=stft(
        audio,
        fs=sr,
        nperseg=4096
    )


    magnitude=np.abs(Z)


    plt.figure(figsize=(12,6))

    plt.pcolormesh(
        t,
        f,
        20*np.log10(
            magnitude+1e-10
        ),
        shading="auto"
    )


    plt.ylim(
        0,
        min(
            20000,
            sr/2
        )
    )


    plt.title(
        "Audio Spectrogram"
    )

    plt.xlabel(
        "Seconds"
    )

    plt.ylabel(
        "Frequency Hz"
    )

    plt.colorbar(
        label="dB"
    )


    plt.show()



# ============================
# DEBUG VIEW
# ============================

def plot_detection(
    audio,
    envelope,
    sr,
    tones
):

    time=np.arange(len(audio))/sr


    plt.figure(figsize=(14,5))


    plt.plot(
        time,
        envelope,
        label="Envelope"
    )


    for s,e,_ in tones:

        plt.axvspan(
            s,
            e,
            alpha=.3
        )


    plt.title(
        "Tone Detection"
    )

    plt.xlabel(
        "Seconds"
    )

    plt.legend()

    plt.show()



# ============================
# MAIN
# ============================

def main():


    audio,sr=load_audio(
        AUDIO_FILE
    )


    envelope=get_envelope(
        audio,
        sr
    )


    tones=detect_tones(
        envelope,
        sr
    )


    gaps=extract_gaps(
        tones
    )


    unit=estimate_unit(
        gaps
    )


    bits=decode_gap_bits(
        gaps,
        unit
    )


    print(
        "\n[+] Hidden bits:"
    )

    print(
        "".join(map(str,bits))
    )


    print(
        "\n[+] ASCII candidates:"
    )


    for text in bits_to_ascii(bits):

        print(
            text
        )


    # Save measurements

    df=pd.DataFrame(
        {
        "gap_seconds":gaps,
        "bit":bits
        }
    )


    df.to_csv(
        OUTPUT_CSV,
        index=False
    )


    print(
        f"\n[+] Saved {OUTPUT_CSV}"
    )


    plot_detection(
        audio,
        envelope,
        sr,
        tones
    )


    analyze_spectrum(
        audio,
        sr
    )



if __name__=="__main__":
    main()