#!/usr/bin/env python3
"""
model_trainer.py

- Finds the highest-numbered CSV in data/ (or uses numeric argv override)
- CSV format: gesture_name,take_number,seq_num,val1,...
- Groups rows by (gesture_name, take_number) to form 'takes' (each take -> sequence of sensor rows)
- Builds a small MLP: Flatten -> Dense(64, relu) -> Dense(num_classes, softmax)
- Trains the model and after each epoch converts & saves an 8-bit quantized TFLite model
"""

import os
import sys
import glob
import csv
import collections
import math
import random
from typing import List, Tuple, Dict
import datetime
import shutil

import numpy as np
import tensorflow as tf

DATA_DIR = "data"
MODELS_DIR = "models"
SEED = 42
BATCH_SIZE = 8
EPOCHS = 20
HIDDEN_UNITS = 64
TEST_SPLIT = 0.2
csv_vals = 6 # could be a constant but this way it's more 'scalable'

np.random.seed(SEED)
tf.random.set_seed(SEED)
random.seed(SEED)


def find_highest_csv_numbered(data_dir: str) -> str:
    """Return basename (without dir) of highest numeric csv, e.g. '7.csv' -> '7'"""
    files = glob.glob(os.path.join(data_dir, "*.csv"))
    if not files:
        raise FileNotFoundError(f"No CSV files found in {data_dir}")
    numeric_files = []
    for f in files:
        base = os.path.basename(f)
        name, ext = os.path.splitext(base)
        if ext.lower() != ".csv":
            continue
        try:
            n = int(name)
            numeric_files.append((n, f))
        except ValueError:
            # ignore non-numeric names
            pass
    if not numeric_files:
        raise FileNotFoundError(f"No numerically-named CSVs found in {data_dir}")
    numeric_files.sort(key=lambda x: x[0])
    chosen = numeric_files[-1][1]
    return chosen


def load_csv_to_takes(csv_path: str):
    """
    Parse CSV and return:
      - takes: dict[(gesture_name, take_number)] -> list of (seq_num, [CSV_VALS floats])
      - sensor_count_per_take: mapping of take -> length
    """
    takes = collections.defaultdict(list)
    print(f"Loading CSV: {csv_path}")
    with open(csv_path, newline="") as fh:
        reader = csv.reader(fh)
        header = next(reader, None)
        if header is None:
            raise ValueError("CSV is empty")
        # header expected: gesture_name,take_number,seq_num,val1...val{csv_vals}
        for i, row in enumerate(reader):
            if not row:
                continue
            # trim whitespace
            row = [c.strip() for c in row]
            # if len(row) < 9:
            #     raise ValueError(f"Row {i+2} has too few columns: {row}")
            gesture = row[0]
            take_num = row[1]
            seq_num = int(row[2])
            vals = [float(x) for x in row[3:]]
            csv_vals = len(vals)
            takes[(gesture, take_num)].append((seq_num, vals))
    # sort each take by seq_num
    for k in takes:
        takes[k].sort(key=lambda x: x[0])
    lens = [len(v) for v in takes.values()]
    print(f"Found {len(takes)} takes from {len(set([k[0] for k in takes.keys()]))} gestures")
    print(f"Take lengths (min, mode, max): {min(lens)}, {mode(lens):.0f}, {max(lens)}")
    return takes


def mode(xs: List[int]) -> int:
    counts = collections.Counter(xs)
    return counts.most_common(1)[0][0]


def buildsamples_from_takes(takes: Dict[Tuple[str, str], List[Tuple[int, List[float]]]], target_length=None):
    """
    Convert takes -> X, y arrays:
      - Each take gives a single sample: sequence_length x CSV_VALS -> flattened vector (sequence_length*CSV_VALS,)
      - If takes have variable length: pad with zeros or truncate to target_length.
    Returns:
      X: np.array (N, target_length*CSV_VALS)
      y: np.array (N,) integer labels
      label_map: dict label_name -> integer
    """
    # determine target length if not provided: use mode length
    lengths = [len(v) for v in takes.values()]
    if not target_length:
        target_length = mode(lengths)
    print(f"Using target sequence length per take = {target_length}")

    # build label map
    gestures = sorted(set(k[0] for k in takes.keys()))
    label_map = {g: i for i, g in enumerate(gestures)}
    print("Label map:", label_map)

    samples = []
    labels = []
    for (gesture, take_id), rows in takes.items():
        seq_vals = [vals for (_, vals) in rows]
        # pad or truncate
        if len(seq_vals) < target_length:
            # pad with zeros at the end
            pad_count = target_length - len(seq_vals)
            seq_vals = seq_vals + [[0.0]*csv_vals]*pad_count
        elif len(seq_vals) > target_length:
            seq_vals = seq_vals[:target_length]
        arr = np.array(seq_vals, dtype=np.float32).reshape(-1)  # flattened
        samples.append(arr)
        labels.append(label_map[gesture])
    X = np.stack(samples, axis=0)
    y = np.array(labels, dtype=np.int32)
    print(f"Built dataset: X.shape={X.shape}, y.shape={y.shape}")
    return X, y, label_map, target_length


def stratified_train_test_split(X: np.ndarray, y: np.ndarray, test_ratio=0.2, seed=SEED):
    """Simple stratified split by label preserving takes per-class."""
    rng = np.random.RandomState(seed)
    indices_by_label = {}
    for i, label in enumerate(y):
        indices_by_label.setdefault(int(label), []).append(i)
    train_idx = []
    test_idx = []
    for label, idxs in indices_by_label.items():
        rng.shuffle(idxs)
        n_test = max(1, int(len(idxs) * test_ratio))
        test_idx.extend(idxs[:n_test])
        train_idx.extend(idxs[n_test:])
    # shuffle final
    rng.shuffle(train_idx)
    rng.shuffle(test_idx)
    X_train = X[train_idx]
    y_train = y[train_idx]
    X_test = X[test_idx]
    y_test = y[test_idx]
    print(f"Train samples: {len(train_idx)}, Test samples: {len(test_idx)}")
    return X_train, X_test, y_train, y_test


# ---------------------------
# TFLite conversion with int8 quantization
# ---------------------------
def convert_to_int8_tflite(keras_model: tf.keras.Model, representative_data_gen, output_path: str):
    """
    Convert a Keras model -> int8 TFLite model suitable for TFLite Micro.
    representative_data_gen: generator that yields calibration samples
                             -> must yield {input_name: np.array(dtype=float32)}
    """
    tmp_saved = "tmp_saved_model"
    if os.path.exists(tmp_saved):
        shutil.rmtree(tmp_saved)

    keras_model.export(tmp_saved)

    converter = tf.lite.TFLiteConverter.from_saved_model(tmp_saved)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    converter.representative_dataset = representative_data_gen

    try:
        tflite_model = converter.convert()
    except Exception as e:
        print("TFLite conversion failed:", e)
        raise

    with open(output_path, "wb") as f:
        f.write(tflite_model)
    print(f"Saved quantized TFLite model to {output_path}")

    shutil.rmtree(tmp_saved)


# ---------------------------
# Build model
# ---------------------------
def build_mlp(input_dim: int, num_classes: int, hidden_units=HIDDEN_UNITS):
    inputs = tf.keras.Input(shape=(input_dim,), name="input")
    x = tf.keras.layers.Dense(hidden_units, activation="relu", name="hidden")(inputs)
    outputs = tf.keras.layers.Dense(num_classes, activation="softmax", name="logits")(x)
    model = tf.keras.Model(inputs=inputs, outputs=outputs, name="small_mlp")
    model.compile(
        optimizer=tf.keras.optimizers.Adam(),
        loss='sparse_categorical_crossentropy',
        metrics=['accuracy']
    )
    return model


# ---------------------------
# Representative dataset generator factory
# ---------------------------
def make_representative_dataset(X_train: np.ndarray, num_samples=100):
    """
    Returns a function for converter.representative_dataset that yields float32 input arrays.
    """
    # pick up to num_samples random train samples
    samples = X_train.copy()
    if len(samples) > num_samples:
        idxs = np.random.choice(len(samples), num_samples, replace=False)
        samples = samples[idxs]
    def gen():
        for s in samples:
            # converter expects a list/iterator yielding dict or list of arrays matching inputs
            # we yield a single-input model array shaped (1, input_dim) as float32
            arr = np.expand_dims(s.astype(np.float32), axis=0)
            yield [arr]
    return gen


# ---------------------------
# Main
# ---------------------------
def main():
    # pick csv file
    override = None
    if len(sys.argv) > 1:
        override = sys.argv[1].strip()
    if override:
        csv_candidate = os.path.join(DATA_DIR, f"{override}.csv")
        if not os.path.exists(csv_candidate):
            raise FileNotFoundError(f"Requested CSV {csv_candidate} not found")
        csv_file = csv_candidate
    else:
        csv_file = find_highest_csv_numbered(DATA_DIR)

    # load takes
    takes = load_csv_to_takes(csv_file)
    # determine target length: use mode of lengths (function does that)
    X_all, y_all, label_map, seq_len = buildsamples_from_takes(takes, target_length=None)
    input_dim = X_all.shape[1]
    num_classes = len(label_map)
    assert num_classes >= 2, "Need at least 2 gesture classes"

    # train/test split (stratified by take)
    X_train, X_test, y_train, y_test = stratified_train_test_split(X_all, y_all, test_ratio=TEST_SPLIT)

    # normalize: simple per-feature zero mean unit var using training stats
    mean = X_train.mean(axis=0, keepdims=True)
    std = X_train.std(axis=0, keepdims=True) + 1e-9
    print("Applying standard normalization (train mean/std).")
    X_train_norm = (X_train - mean) / std
    X_test_norm = (X_test - mean) / std

    # build model
    model = build_mlp(input_dim=input_dim, num_classes=num_classes)
    model.summary(print_fn=print)

    # prepare directories
    os.makedirs(MODELS_DIR, exist_ok=True)

    # representative dataset generator (uses normalized float32 inputs)
    rep_gen = make_representative_dataset(X_train_norm, num_samples=100)

    # Train
    print(f"Starting training: epochs={EPOCHS}, batch_size={BATCH_SIZE}")
    history = model.fit(
        X_train_norm, y_train,
        validation_data=(X_test_norm, y_test),
        epochs=EPOCHS,
        batch_size=BATCH_SIZE,
        verbose=1,
        shuffle=True
    )

    # Final evaluation
    eval_res = model.evaluate(X_test_norm, y_test, verbose=0)
    print(f"Final evaluation on test set: loss={eval_res[0]:.4f}, acc={eval_res[1]:.4f}")

    # After training loop
    now = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    tflite_path = os.path.join(MODELS_DIR, f"{now}.tflite")
    print(f"\nConverting final model to int8 TFLite -> {tflite_path} ...")
    convert_to_int8_tflite(model, representative_data_gen=rep_gen, output_path=tflite_path)

if __name__ == "__main__":
    main()
