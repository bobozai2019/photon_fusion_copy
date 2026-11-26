extends RefCounted

static var active := false
static var calls := 0
static var replica_us := 0
static var read_us := 0
static var sync_us := 0
static var apply_us := 0
static var advance_us := 0
