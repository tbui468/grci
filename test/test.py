import grci
import builtin_modules
import basic

total = 0
passed = 0
failed = 0

def test_module(name, cases):
    global total, failed, passed

    module = grci.Module(name)
    ok = True
    for c in cases:
        c = c.replace(" ", "")
        if len(c) != module.input_count + module.output_count:
            print("Module " + name + " test case has invalid input/output counts")
            total += 1
            failed += 1
        for i, v in enumerate(c):
            if i == module.input_count:
                break
            module.inp[i] = True if "1" == v else False

        module.step()

        ok = True         
        for i, v in enumerate(c[module.input_count:]):
            expected = True if "1" == v else False
            if module.out[i] != expected:
                ok = False
                break

        if not ok:
            break

    if ok:
        passed += 1
    else:
        failed += 1
    total += 1

def test_file(tests, hdl_path):
    grci.init()

    if not hdl_path == None:
        with open(hdl_path, "r") as f:
            src = f.read()
            grci.compile_src(src)

    for t in tests:
        test_module(t[0], t[1])

    grci.quit()


test_file(builtin_modules.tests, None)
test_file(basic.tests, "test.hdl")


print(str(passed) + "/" + str(total))

