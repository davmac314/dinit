const std = @import("std");

const a = std.heap.smp_allocator;

var service_dirs: std.ArrayListUnmanaged([:0]const u8) = .empty;

var services: std.StringArrayHashMapUnmanaged([:0]const u8) = .empty;

pub fn main() !void {
    try load_definitions();

    if (std.os.argv.len == 1) try list_all();
    if (std.os.argv.len == 2) try edit(std.mem.span(std.os.argv[1]));
}

pub fn load_definitions() !void {
    const uid = std.posix.system.getuid();

    if (uid == 0) {
        var default_system = [_][:0]const u8{ "/etc/dinit.d", "/run/dinit.d", "/usr/local/lib/dinit.d", "/lib/dinit.d" };
        service_dirs = .fromOwnedSlice(&default_system);
    } else {
        service_dirs = try .initCapacity(a, 5);
        if (std.posix.getenv("XDG_CONFIG_HOME")) |prefix| {
            service_dirs.appendAssumeCapacity(try std.fmt.allocPrintZ(a, "{s}/dinit.d", .{prefix}));
        }
        if (std.posix.getenv("HOME")) |prefix| {
            service_dirs.appendAssumeCapacity(try std.fmt.allocPrintZ(a, "{s}/.config/dinit.d", .{prefix}));
        }
        service_dirs.appendSliceAssumeCapacity(&.{ "/etc/dinit.d/user", "/usr/lib/dinit.d/user", "/usr/local/lib/dinit.d/user" });
    }

    for (service_dirs.items) |dir_path| {
        const dir = std.fs.openDirAbsoluteZ(dir_path, .{ .iterate = true }) catch |e| {
            if (e == error.FileNotFound) continue;
            return e;
        };
        var iter = dir.iterate();
        while (try iter.next()) |entry| {
            // std.log.info("{s}/{s}", .{ dir_path, entry.name });
            if (entry.kind == .file and !services.contains(entry.name)) {
                try services.put(a, try a.dupe(u8, entry.name), try std.fmt.allocPrintZ(a, "{s}/{s}", .{ dir_path, entry.name }));
            }
        }
    }
}

fn list_all() !void {
    const stdout = std.io.getStdOut();
    var longest: usize = 0;
    var iter = services.iterator();
    while (iter.next()) |entry| {
        longest = @max(longest, entry.key_ptr.len);
    }
    const pad_buffer: [36]u8 = @splat(' ');
    iter = services.iterator();
    while (iter.next()) |entry| {
        const pad = pad_buffer[0 .. longest + 4 - entry.key_ptr.len];
        try stdout.writer().print("{s}{s}{s}\n", .{ entry.key_ptr.*, pad, entry.value_ptr.* });
    }
}

fn edit(service_name: []const u8) !void {
    const editor = std.posix.getenvZ("EDITOR") orelse "/usr/bin/kak";
    var iter = services.iterator();
    while (iter.next()) |entry| {
        if (std.mem.eql(u8, entry.key_ptr.*, service_name)) {
            const command = try std.fmt.allocPrintZ(a, "{s} {s}", .{ editor, entry.value_ptr.* });
            return std.posix.execveZ("/bin/sh", &.{ "sh", "-c", command }, @ptrCast(std.os.environ));
        }
    }
    std.debug.print("Cannot find service file\n", .{});
    std.process.exit(1);
}
